// SPDX-License-Identifier: LGPL-2.1
/*
 * CLI interface for C.
 *
 * Copyright (C) 2022 Steven Rostedt <rostedt@goodmis.org>
 */
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
		errno = -EINVAL;
		return -1;
	}

	cmd = find_command(ccli, command_name);
	if (!cmd) {
		errno = -ENODEV;
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

	argc = line_parse(&copy, &argv);
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
		refresh_line(ccli, line, 0);

		for (i = 0; i < cnt; i++)
			free(list[i]);
		free(list);
	}

	free_argv(argc, argv);
 out:
	line_cleanup(&copy);
	refresh_line(ccli, ccli->line, 0);
}

__hidden void do_completion(struct ccli *ccli, struct line_buf *line, int tab)
{
	struct command *command;
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
		command = &ccli->commands[last];
		i = strlen(command->cmd);
		insert_word(ccli, line, command->cmd + len, i - len);
		line_insert(line, ' ');
		refresh_line(ccli, line, 0);
		goto out_free;
	}

	/* list all the matches if tab was hit more than once */
	if (!tab)
		goto out_free;

	echo(ccli, '\n');

	print_completion(ccli, line->line + s, len,
			  ccli->nr_commands, commands);

	refresh_line(ccli, line, 0);
 out_free:
	free(commands);
}
