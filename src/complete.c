// SPDX-License-Identifier: LGPL-2.1
/*
 * CLI interface for C.
 *
 * Copyright (C) 2022 Steven Rostedt <rostedt@goodmis.org>
 */
#include <sys/ioctl.h>
#include <stdarg.h>
#include <stdio.h>

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

/**
 * ccli_register_default_completion - Register a completion to do for unknown commmands
 * @ccli: The CLI descriptor to register the default completion
 * @completion: The completion function to call for unknown commands
 * @data: The data to pass to that completion.
 *
 * Have @completion be called for the first word parsing that doesn't have a
 * command registered for it.
 *
 * Returns 0 on success and -1 on error.
 */
int ccli_register_default_completion(struct ccli *ccli, ccli_completion completion,
				     void *data)
{
	ccli->default_completion = completion;
	ccli->default_completion_data = data;
	return 0;
}

static void print_completion_flat(struct ccli *ccli, const char *match,
				  int len, int nr_str, char **strings, int index)
{
	int i;

	for (i = 0; i < nr_str; i++) {
		if (strncmp(match, strings[i], len) == 0) {
			echo_str(ccli, strings[i] + index);
			echo(ccli, '\n');
		}
	}
}

static void print_string_list(struct ccli *ccli, char **list, int cnt, int max_len,
			      struct winsize *w)
{
	char *spaces;
	char *str;
	int cols, rows;
	int index;
	int i, x, c = 0;

	index = ccli->display_index;

	if (!max_len) {
		for (i = 0; i < cnt; i++) {
			int len = strlen(list[i]);

			if (len > max_len)
				max_len = len;
		}
	}

	spaces = malloc(max_len);
	memset(spaces, ' ', max_len);

	cols = w->ws_col;
	cols = cols / (max_len + 2);
	if (!cols)
		cols = 1;

	rows = (cnt + cols - 1) / cols;

	for (i = 0; i < rows; i++) {
		char ans = 0;;

		if (check_for_ctrl_c(ccli))
			break;

		if (!c && i && !(i % (w->ws_row - 1))) {
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
			if (x * rows + i >= cnt)
				continue;
			if (x)
				echo_str(ccli, "  ");
			str = list[x * rows + i];
			str += index;
			echo_str(ccli, str);
			if (strlen(str) < max_len)
				echo_str_len(ccli, spaces, max_len - strlen(str));
		}
		echo(ccli, '\n');
	}
	free(spaces);

}

int ccli_print_list(struct ccli *ccli, char **list, int cnt, int max_len)
{
	struct winsize w;
	int ret;

	ret = ioctl(ccli->in, TIOCGWINSZ, &w);
	if (ret < 0)
		return -1;

	print_string_list(ccli, list, cnt, max_len, &w);
	return 0;
}

static void print_completion(struct ccli *ccli, const char *match,
			     int len, int nr_str, char **strings, int index)
{
	struct winsize w;
	char *str;
	int max_len = 0;
	int ret;
	int i, x;

	if (index > len)
		index = len;

	if (!ccli->in_tty)
		return print_completion_flat(ccli, match, len,
					     nr_str, strings, index);

	ret = ioctl(ccli->in, TIOCGWINSZ, &w);
	if (ret)
		return print_completion_flat(ccli, match, len,
					     nr_str, strings, index);

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

	max_len -= index;

	print_string_list(ccli, strings, nr_str, max_len, &w);
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

static int do_strcmp(const void *A, const void *B)
{
	char * const *a = A;
	char * const *b = B;

	/* Handle NULLs (they go at the end) */
	if (!*a || !*b) {
		if (*a)
			return -1;
		return !!(*b);
	}

	return strcmp(*a, *b);
}

static int sort_unique(char **list, int cnt)
{
	int l;
	int i;

	qsort(list, cnt, sizeof(char *), do_strcmp);

	for (l = 0, i = 0; i < cnt && list[i]; i++) {
		if (!i)
			continue;
		if (strcmp(list[l], list[i]) != 0) {
			l++;
			if (l != i) {
				free(list[l]);
				list[l] = list[i];
				list[i] = NULL;
			}
		}
	}

	if (l < cnt && list[l])
		l++;

	for (i = l; i < cnt; i++) {
		free(list[i]);
		list[i] = NULL;
	}

	return l;
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

static void do_completion_table(struct ccli *ccli, int argc, char **argv, int word,
				char ***list, int *cnt, struct line_buf *line,
				char *match)
{
	const struct ccli_completion_table *table = ccli->completion_table;
	struct line_buf copy;
	struct command *cmd;
	void *data;
	int i, c;

	if (!table)
		return;

	/* Iterate down finding the node that matches this word */
	for (i = 0; i < word; i++) {
		for (c = 0; table->options[c]; c++) {
			const char *name = table->options[c]->name;
			if (strcmp(argv[i], name) == 0)
				break;
		}
		if (!table->options[c])
			break;
		table = table->options[c];
	}

	/* Always call the completion callback for a matched node */
	if (table->completion) {
		int ret;

		data = ccli->completion_table_data;
		cmd = find_command(ccli, argv[0]);
		if (cmd && cmd->data)
			data = cmd->data;

		/* The callback may mess with the line */
		ret = line_copy(&copy, line, line->pos);
		if (ret >= 0)
			ret = table->completion(ccli, argv[0], copy.line, word,
						match, list, data);
		if (ret > 0)
			*cnt += ret;
		line_cleanup(&copy);
	}

	/* Add the options only if we are at the next word to find */
	if (i != word)
		return;

	for (c = 0; table->options[c]; c++)
		ccli_list_add(ccli, list, cnt, table->options[c]->name);

}

__hidden void do_completion(struct ccli *ccli, struct line_buf *line, int tab)
{
	struct command *cmd = NULL;
	struct line_buf copy;
	char **list = NULL;
	char empty[1] = "";
	char **argv;
	char *match;
	char delim;
	int matched = 0;
	int index;
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
	if (argc < 0)
		goto out;

	word = argc - 1;

	/* If the cursor is on a space, there's no word to match */
	if (word < 0 || ISSPACE(copy.line[copy.pos - 1])) {
		match = empty;
		word++;
	} else {
		match = argv[word];
	}

	mlen = strlen(match);

	/* Do the registered completions first */
	if (word) {
		cmd = find_command(ccli, argv[0]);
		if (cmd && cmd->completion)
			cnt = cmd->completion(ccli, cmd->cmd, copy.line, word,
					      match, &list, cmd->data);
	}

	/*
	 * Next do the default completion operation
	 * if command completion was not done
	 */
	if ((!cmd || !cmd->completion) && ccli->default_completion)
		cnt = ccli->default_completion(ccli, NULL, copy.line, word,
					       match, &list,
					       ccli->default_completion_data);

	/* As match may be read again, need to put it back together*/
	delim = match[mlen];
	match[mlen] = '\0';

	/* Next do the default completion operation if command completion was not done */
	if ((!cmd || !cmd->completion) && ccli->default_completion)
		cnt = ccli->default_completion(ccli, NULL, copy.line, word,
					       match, &list,
					       ccli->default_completion_data);
	if (delim == '\0')
		delim = match[mlen];
	match[mlen] = '\0';

	/* End with the commands */
	if (!word) {
		/* This is a list of commands */
		for (i = 0; i < ccli->nr_commands; i++)
			ccli_list_add(ccli, &list, &cnt, ccli->commands[i].cmd);
	}

	if (delim == '\0')
		delim = match[mlen];
	match[mlen] = '\0';

	do_completion_table(ccli, argc, argv, word, &list, &cnt, line, match);

	index = ccli->display_index;

	if (delim == '\0')
		delim = match[mlen];
	match[mlen] = '\0';
	if (!delim)
		delim = ' ';

	/* If nothing was matched yet */
	if (cnt >= 0 && !word) {
		/* Try matching with the list of commands */
		for (i = 0; i < ccli->nr_commands; i++)
			ccli_list_add(ccli, &list, &cnt, ccli->commands[i].cmd);
	}

	if (cnt < 0)
		goto free_args;

	index = ccli->display_index;

	cnt = sort_unique(list, cnt);
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
		print_completion(ccli, match, mlen, cnt, list, index);
	}
	line_refresh(ccli, line, 0);

	for (i = 0; i < cnt; i++)
		free(list[i]);
	free(list);

 free_args:
	free_argv(argc, argv);
 out:
	ccli->display_index = 0;
	line_cleanup(&copy);
	line_refresh(ccli, ccli->line, 0);
}

static void test_table(const void *data)
{
	const struct ccli_completion_table *table = data;
	const char *name = table->name;
	int len = strlen(test_name);
	int l = 0;
	int i;

	/* The root of the completion table does not need a name */
	if (!name)
		name = "root";

	if (len)
		strncat(test_name, "->", BUFSIZ - 1);
	strncat(test_name, name, BUFSIZ - 1);

	snprintf(test_message, BUFSIZ - 1,
		 "Completion table missing name or 'options' NULL terminator");

	/* Touch all the names first */
	for (i = 0; table->options[i]; i++)
		l += strlen(table->options[i]->name);

	/* Now recurse */
	for (i = 0; table->options[i]; i++)
		test_table(table->options[i]);

	test_name[len] = '\0';
}

/**
 * ccli_register_completion_table - register a completion table
 * @ccli: The CLI descriptor to regsiter a completion table to
 * @table: The completion table to assign.
 * @data: data that can be passed if the command does not have one.
 *
 * The @table is the root to hold other completions, and its name is ignored.
 * Each entry in the table must have its options array end with NULL. Even
 * if it is a leaf node and has no more children.
 *
 * The completion field of an entry will be called if its name matched on
 * the previous word.
 */
int ccli_register_completion_table(struct ccli *ccli,
				   const struct ccli_completion_table *table,
				   void *data)
{
	bool crashed;

	/*
	 * Need to walk the completion table to make sure that
	 * every element has populated the "options" field with a
	 * NULL terminated array. Check here to remove unwanted surprises
	 * later. If it crashes, it will report the problem, and not
	 * register the table.
	 */
	crashed = test_for_crash(test_table, table);

	if (crashed) {
		errno = EFAULT;
		return -1;
	}

	ccli->completion_table = table;
	ccli->completion_table_data = data;

	return 0;
}
