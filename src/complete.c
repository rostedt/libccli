// SPDX-License-Identifier: LGPL-2.1
/*
 * CLI interface for C.
 *
 * Copyright (C) 2022 Steven Rostedt <rostedt@goodmis.org>
 */
#include <stdarg.h>
#include <sys/ioctl.h>

#include "ccli-local.h"

/**
 * ccli_register_completion - Register a completion for command
 * @ccli: The CLI descriptor to register a completion for.
 * @command_name: The command that the completion is for.
 * @completion: The completion function to call.
 *
 * Register a @completion function to be called when the user had
 * typed a command and hits tab on one of its pramaters.
 *
 * Returns 0 on success and -1 on error.
 */
int ccli_register_completion(struct ccli *ccli, const char *command_name,
			     ccli_completion completion)
{
	struct command *cmd;

	if (!ccli || !command_name || !completion) {
		errno = EINVAL;
		return -1;
	}

	cmd = find_command(ccli, command_name);
	if (!cmd) {
		errno = ENODEV;
		return -1;
	}

	cmd->completion = completion;
	return 0;
}

static void print_completion_flat(struct ccli *ccli, const char *match,
				  int len, int nr_str, char **strings)
{
	int i;

	for (i = 0; i < nr_str; i++) {
		if (strncmp(match, strings[i], len) == 0) {
			echo_str(ccli, strings[i]);
			echo(ccli, '\n');
		}
	}
}

static void print_completion(struct ccli *ccli, const char *match,
			     int len, int nr_str, char **strings)
{
	struct winsize w;
	char *spaces;
	char *str;
	int max_len = 0;
	int cols, rows;
	int i, x, c = 0;
	int ret;

	if (!ccli->in_tty)
		return print_completion_flat(ccli, match, len,
					     nr_str, strings);

	ret = ioctl(ccli->in, TIOCGWINSZ, &w);
	if (ret)
		return print_completion_flat(ccli, match, len,
					     nr_str, strings);
	cols = w.ws_col;

	for (i = 0, x = 0; i < nr_str; i++) {
		if (strncmp(match, strings[i], len) == 0) {
			if (strlen(strings[i]) > max_len)
				max_len = strlen(strings[i]);
			if (i != x) {
				/* Keep all matches at the front */
				str = strings[x];
				strings[x] = strings[i];
				strings[i] = str;
			}
			x++;
		}
	}
	if (!max_len)
		return;

	spaces = malloc(max_len);
	memset(spaces, ' ', max_len);

	nr_str = x;

	cols = cols / (max_len + 2);
	if (!cols)
		cols = 1;

	rows = (nr_str + cols - 1) / cols;

	for (i = 0; i < rows; i++) {
		char ans = 0;;

		if (check_for_ctrl_c(ccli))
			break;

		if (!c && i && !(i % (w.ws_row - 1))) {
			ans = page_stop(ccli);
			switch (ans) {
			case 'q':
				i = rows;
				continue;
			case 'c':
				c = 1;
				break;
			}
		}
		for (x = 0; x < cols; x++) {
			if (x * rows + i >= nr_str)
				continue;
			if (x)
				echo_str(ccli, "  ");
			str = strings[x * rows + i];
			echo_str(ccli, str);
			if (strlen(str) < max_len)
				echo_str_len(ccli, spaces, max_len - strlen(str));
		}
		echo(ccli, '\n');
	}
	free(spaces);
}

/*
 * return the number of characters that match between @a and @b.
 */
static int match_chars(const char *a, const char *b)
{
	int i;

	for (i = 0; a[i] && b[i]; i++) {
		if (a[i] != b[i])
			break;
	}
	return i;
}

static int find_matches(const char *match, int mlen, char **list, int cnt,
			int *last_match, int *max)
{
	int max_match = -1;
	int matched = 0;
	int i, x, l, m = -1;

	for (i = 0; i < cnt; i++) {
		/* If list[i] failed to allocate, we need to handle that */
		if (!list[i])
			continue;
		if (!mlen || strncmp(list[i], match, mlen) == 0) {
			if (m >= 0) {
				x = match_chars(list[m], list[i]);
				if (max_match < 0 || x < max_match)
					max_match = x;
			} else {
				m = i;
			}
			matched++;
			l = i;
		}
	}
	*last_match = l;
	*max = max_match;
	return matched;
}

static void insert_word(struct ccli *ccli, struct line_buf *line,
			const char *word, int len)
{
	int i;

	for (i = 0; i < len; i++)
		line_insert(line, word[i]);
}

/* Allocate new lists in 64 word blocks */
#define LIST_BLK	64
#define LIST_MASK	(LIST_BLK - 1)

static char **update_list(char ***list, int size)
{
	char **words = *list;

	if (!(size & LIST_MASK)) {
		/* (size + 1 + LIST_BLK - 1) & ~(LIST_MASK) */
		size = (size + LIST_BLK) & ~(LIST_MASK);
		/* Add two to be on the safe side */
		words = realloc(words, sizeof(*words) * (size + 2));
		if (!words)
			return NULL;
		*list = words;
	}
	return words;
}

/**
 * ccli_list_add - add an copy of a word to the completion list
 * @ccli: The CLI descriptor.
 * @list: The list to add to.
 * @cnt: The current count of items in the list (do not touch)
 * @word: The word to add to the list.
 *
 * Add a @word to the completion list. It will allocate and copy the
 * @word. If it fails the allocation of the copy, it will continue as
 * the list can handle NULL entries.
 *
 * The @cnt must be initialized to zero, and not touched by the application.
 * The only modifications to @cnt should be done by one of the list
 * helper functions.
 *
 * Returns the new size of the list on success (even if it fails to
 *   do the copy, but it does increase the size of the list.
 *
 * Returns -1 if it failed to increase the size of the list.
 */
int ccli_list_add(struct ccli *ccli, char ***list, int *cnt, const char *word)
{
	char **words;
	int size = *cnt;

	words = update_list(list, size);
	if (!words)
		return -1;

	words[size] = strdup(word);
	/* It's OK if it fails, we can handle it. */

	*cnt = size + 1;
	return *cnt;
}

/**
 * ccli_list_insert - add a pointer to a word to the completion list
 * @ccli: The CLI descriptor.
 * @list: The list to add to.
 * @cnt: The current count of items in the list (do not touch)
 * @word: The word to add to the list.
 *
 * Add a @word to the completion list. It will add the pointer to @word
 * to the list. The difference between this and ccli_list_add() is that
 * ccli_list_add() will allocate a copy of @word, where this will use
 * @word itself to add to the list.
 *
 * Note, @word will be freed when the list is freed.
 *
 * The @cnt must be initialized to zero, and not touched by the application.
 * The only modifications to @cnt should be done by one of the list
 * helper functions.
 *
 * Returns the new size of the list on success, or -1 if it failed to
 *   increase the size of the list.
 */
int ccli_list_insert(struct ccli *ccli, char ***list, int *cnt, char *word)
{
	char **words;
	int size = *cnt;

	words = update_list(list, size);
	if (!words)
		return -1;

	words[size] = word;

	*cnt = size + 1;
	return *cnt;
}

/**
 * ccli_list_add_printf - add a string to the list
 * @ccli: The CLI descriptor.
 * @list: The list to add to.
 * @cnt: The current count of items in the list (do not touch)
 * @fmt: The printf format to add
 *
 * Add a string to the completion list that is defined by the fmt
 * and parameters.
 *
 * The @cnt must be initialized to zero, and not touched by the application.
 * The only modifications to @cnt should be done by one of the list
 * helper functions.
 *
 * Returns the new size of the list on success (even if it fails to
 *   allocate the string, but it does increase the size of the list.
 *
 * Returns -1 if it failed to increase the size of the list.
 */
int ccli_list_add_printf(struct ccli *ccli, char ***list, int *cnt, const char *fmt, ...)
{
	va_list ap;
	char **words;
	int size = *cnt;

	words = update_list(list, size);
	if (!words)
		return -1;

	va_start(ap, fmt);
	vasprintf(words + size, fmt, ap);
	va_end(ap);
	/* It's OK if it fails, we can handle it. */

	*cnt = size + 1;
	return *cnt;
}

/**
 * ccli_list_free - free a completion list
 * @ccli: The CLI descriptor.
 * @list: The list to free.
 * @cnt: The number of items on the list.
 *
 * Will free the @list.
 */
void ccli_list_free(struct ccli *ccli, char ***list, int cnt)
{
	char **words = *list;
	int i;

	if (!words)
		return;

	for (i = 0; i < cnt; i++)
		free(words[i]);
	free(words);
	*list = NULL;
}

static void word_completion(struct ccli *ccli, struct line_buf *line, int tab)
{
	struct command *cmd;
	struct line_buf copy;
	char **list = NULL;
	char empty[1] = "";
	char **argv;
	char *match;
	char delim;
	int matched = 0;
	int word;
	int argc;
	int mlen;
	int last;
	int max;
	int len;
	int cnt = 0;
	int ret;
	int i;

	ret = line_copy(&copy, line, line->pos);
	if (ret < 0)
		return;

	argc = line_parse(copy.line, &argv);
	if (argc <= 0)
		goto out;

	word = argc - 1;

	/* If the cursor is on a space, there's no word to match */
	if (ISSPACE(copy.line[copy.pos - 1])) {
		match = empty;
		word++;
	} else {
		match = argv[word];
	}

	mlen = strlen(match);

	cmd = find_command(ccli, argv[0]);
	if (cmd && cmd->completion)
		cnt = cmd->completion(ccli, cmd->cmd, copy.line, word,
				      match, &list, cmd->data);

	delim = match[mlen];
	match[mlen] = '\0';
	if (!delim)
		delim = ' ';

	if (cnt) {
		matched = find_matches(match, mlen, list, cnt, &last, &max);

		if (matched == 1) {
			len = strlen(list[last]);
			insert_word(ccli, line, list[last] + mlen, len - mlen);
			if (delim != CCLI_NOSPACE)
				line_insert(line, delim);
		}

		if (matched > 1 && max > mlen)
			insert_word(ccli, line, list[last] + mlen, max - mlen);

		if (tab && matched > 1) {
			echo(ccli, '\n');
			print_completion(ccli, match, mlen, cnt, list);
		}
		line_refresh(ccli, line, 0);

		for (i = 0; i < cnt; i++)
			free(list[i]);
		free(list);
	}

	free_argv(argc, argv);
 out:
	line_cleanup(&copy);
	line_refresh(ccli, ccli->line, 0);
}

__hidden void do_completion(struct ccli *ccli, struct line_buf *line, int tab)
{
	char **commands;
	int matched;
	int last;
	int len;
	int max;
	int i = line->pos - 1;
	int s;

	/* Completion currently only works with the first word */
	while (i >= 0 && !ISSPACE(line->line[i]))
		i--;

	s = i + 1;

	while (i >= 0 && ISSPACE(line->line[i]))
		i--;

	/* If the pos was at the first word, i will be less than zero */
	if (i >= 0)
		return word_completion(ccli, line, tab);

	len = line->pos - s;

	commands = calloc(ccli->nr_commands, sizeof(char *));
	if (!commands)
		return;

	for (i = 0; i < ccli->nr_commands; i++)
		commands[i] = ccli->commands[i].cmd;

	/* Find how many commands match */
	matched = find_matches(line->line + s, len, commands,
			       ccli->nr_commands, &last, &max);
	if (!matched)
		goto out_free;

	if (matched == 1) {
		/* select it */
		i = strlen(commands[last]);
		insert_word(ccli, line, commands[last] + len, i - len);
		line_insert(line, ' ');
		line_refresh(ccli, line, 0);
		goto out_free;
	}

	/* list all the matches if tab was hit more than once */
	if (!tab)
		goto out_free;

	echo(ccli, '\n');

	print_completion(ccli, line->line + s, len,
			  ccli->nr_commands, commands);

	line_refresh(ccli, line, 0);
 out_free:
	free(commands);
}
