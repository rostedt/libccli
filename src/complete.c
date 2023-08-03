// SPDX-License-Identifier: MIT
/*
 * CLI interface for C.
 *
 * Copyright (C) 2022 Steven Rostedt <rostedt@goodmis.org>
 */
#include <sys/ioctl.h>
#include <signal.h>
#include <setjmp.h>
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
 * command or other completion registered for it.
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

static void print_completion(struct ccli *ccli, const char *match,
			     int len, int nr_str, char **strings, int index)
{
	struct winsize w;
	char *spaces;
	char *str;
	int max_len = 0;
	int cols, rows;
	int i, x, c = 0;
	int ret;

	if (index > len)
		index = len;

	if (!ccli->in_tty)
		return print_completion_flat(ccli, match, len,
					     nr_str, strings, index);

	ret = ioctl(ccli->in, TIOCGWINSZ, &w);
	if (ret)
		return print_completion_flat(ccli, match, len,
					     nr_str, strings, index);
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

	nr_str = x;
	max_len -= index;

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
			str += index;
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

static char **realloc_list(char ***list, int size)
{
	char **words;

	/* (size + 1 + LIST_BLK - 1) & ~(LIST_MASK) */
	size = (size + LIST_BLK) & ~(LIST_MASK);
	/* Add two to be on the safe side */
	words = realloc(*list, sizeof(*words) * (size + 2));
	return words;
}

static void free_list(char **list, int cnt)
{
	if (!list)
		return;
	for (int i = 0; i < cnt; i++)
		free(list[i]);
	free(list);
}

static char **update_list(char ***list, int size)
{
	char **words = *list;

	if (!(size & LIST_MASK)) {
		words = realloc_list(list, size);
		if (!words)
			return NULL;
		*list = words;
	}
	return words;
}

static int list_append_and_free(char ***dest, int dsize, char **src, int ssize)
{
	int size = dsize + ssize;
	char **words = *dest;

	if (!src || !ssize) {
		free(src);
		return dsize;
	}

	if (!words) {
		*dest = src;
		return ssize;
	}

	if ((size & ~LIST_MASK) != (dsize & ~LIST_MASK)) {
		words = realloc_list(dest, size);
		if (!words)
			return -1;
		*dest = words;
	}

	memcpy(words + dsize, src, (ssize + 1) * sizeof(*src));
	return size;
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
				char ***clist, int *cnt, struct line_buf *line,
				char *match)
{
	const struct ccli_completion_table *table = ccli->completion_table;
	char **list = NULL;
	struct line_buf copy;
	void *data;
	int i, c;
	int ret = 0;

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
		char *command;

		/* What was registered is the default data */
		data = ccli->completion_table_data;
		/* But the table can override it */
		if (table->data)
			data = table->data;

		command = argc ? argv[0] : "";

		/* The callback may mess with the line */
		ret = line_copy(&copy, line, line->pos);
		if (ret >= 0)
			ret = table->completion(ccli, command, copy.line, word,
						match, &list, data);
		line_cleanup(&copy);
	}

	/* Add the options only if we are at the next word to find */
	if (i == word) {
		for (c = 0; table->options[c]; c++)
			ccli_list_add(ccli, &list, &ret, table->options[c]->name);
	}

	if (ret <= 0)
		return;

	ret = list_append_and_free(clist, *cnt, list, ret);
	if (ret > 0)
		*cnt = ret;
}

static void reset_match(char *delim, int mlen, char *match, const char *save_match)
{
	/*
	 * The completions can update the last character of match to
	 * state not to add a space after a match. If one does, it
	 * happens to all.
	 */
	if (*delim == '\0')
		*delim = match[mlen];
	strcpy(match, save_match);
}

__hidden void do_completion(struct ccli *ccli, struct line_buf *line, int tab)
{
	struct command *cmd = NULL;
	struct line_buf copy;
	char **list = NULL;
	char empty[1] = "";
	char **argv;
	char *save_match;
	char *match = NULL;
	char delim = '\0';
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
		save_match = empty;
		word++;
	} else {
		save_match = argv[word];
	}

	/* Will pass match to the completions to allow them to change it */
	match = strdup(save_match);
	if (!match)
		goto out;

	mlen = strlen(match);

	/* Do the registered completions first */
	if (word) {
		cmd = find_command(ccli, argv[0]);
		if (cmd && cmd->completion)
			cnt = cmd->completion(ccli, cmd->cmd, copy.line, word,
					      match, &list, cmd->data);
	}

	reset_match(&delim, mlen, match, save_match);

	/*
	 * Next do the default completion operation
	 * if command completion was not done
	 */
	if ((!cmd || !cmd->completion) && ccli->default_completion)
		cnt = ccli->default_completion(ccli, NULL, copy.line, word,
					       match, &list,
					       ccli->default_completion_data);

	reset_match(&delim, mlen, match, save_match);

	do_completion_table(ccli, argc, argv, word, &list, &cnt, line, match);

	reset_match(&delim, mlen, match, save_match);

	/* End with the rest of the commands */
	if (!word) {
		/* This is a list of commands */
		for (i = 0; i < ccli->nr_commands; i++)
			ccli_list_add(ccli, &list, &cnt, ccli->commands[i].cmd);
	}

	index = ccli->display_index;

	if (!delim)
		delim = ' ';

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

	free_list(list, cnt);
 free_args:
	free_argv(argc, argv);
 out:
	free(match);
	ccli->display_index = 0;
	line_cleanup(&copy);
	line_refresh(ccli, ccli->line, 0);
}

static sigjmp_buf env;
static char test_table_name[BUFSIZ];
static char test_table_message[BUFSIZ];

static void handler(int sig, siginfo_t *info, void *context)
{
	fprintf(stderr, "ccli: %s\n", test_table_message);
	fprintf(stderr, "      While processing '%s'\n", test_table_name);

	siglongjmp(env, 1);
}

static void test_table(const struct ccli_completion_table *table)
{
	const char *name = table->name;
	int len = strlen(test_table_name);
	int l = 0;
	int i;

	/* The root of the completion table does not need a name */
	if (!name)
		name = "root";

	if (len)
		strncat(test_table_name, "->", BUFSIZ - 1);
	strncat(test_table_name, name, BUFSIZ - 1);

	snprintf(test_table_message, BUFSIZ - 1,
		 "Completion table missing name or 'options' NULL terminator");

	/* Touch all the names first */
	for (i = 0; table->options[i]; i++)
		l += strlen(table->options[i]->name);

	/* Now recurse */
	for (i = 0; table->options[i]; i++)
		test_table(table->options[i]);

	test_table_name[len] = '\0';
}

/**
 * ccli_register_completion_table - register a completion table
 * @ccli: The CLI descriptor to regsiter a completion table to
 * @table: The completion table to assign.
 * @data: The default data to use for completions (if their table doesn't have it)
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
	struct sigaction act = { 0 };
	struct sigaction oldact;
	bool crashed = false;

	/*
	 * Need to walk the completion table to make sure that
	 * every element has populated the "options" field with a
	 * NULL terminated array. Check here to remove unwanted surprises
	 * later. If it crashes, it will report the problem, and not
	 * register the table.
	 */
	act.sa_flags = SA_ONSTACK | SA_SIGINFO;
	sigemptyset(&act.sa_mask);
	act.sa_sigaction = &handler;

	if (sigaction(SIGSEGV, &act, &oldact) < 0)
		return -1;

	if (sigsetjmp(env, 0) == 1) {
		crashed = true;
	} else {
		/*
		 * Make sure all data has options and is NULL.
		 * If not, this should crash!
		 */
		test_table(table);
	}

	sigaction(SIGSEGV, &oldact, NULL);

	if (crashed) {
		errno = EFAULT;
		return -1;
	}

	ccli->completion_table = table;
	ccli->completion_table_data = data;

	return 0;
}
