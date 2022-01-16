// SPDX-License-Identifier: LGPL-2.1
/*
 * CLI interface for C.
 *
 * Copyright (C) 2022 Steven Rostedt <rostedt@goodmis.org>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <termios.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>

#include "ccli.h"

//#define DEBUG
#ifdef DEBUG
#define dprint printf
#else
#define dprint(x...)
#endif

#define DEFAULT_HISTORY_MAX 256

static struct termios savein;
static struct termios saveout;

struct command {
	char			*cmd;
	ccli_command_callback	callback;
	ccli_completion		completion;
	void			*data;
};

struct ccli {
	int			history_max;
	int			history_size;
	int			current_line;
	int			in;
	int			out;
	int			nr_commands;
	char			*prompt;
	struct command		*commands;
	char			**history;
};

static void cleanup(void)
{
	tcsetattr(STDIN_FILENO, TCSANOW, &savein);
	tcsetattr(STDOUT_FILENO, TCSANOW, &saveout);
};

static void echo(struct ccli *ccli, char ch)
{
	write(ccli->out, &ch, 1);
}

static void echo_str(struct ccli *ccli, char *str)
{
	write(ccli->out, str, strlen(str));
}

static void echo_prompt(struct ccli *ccli)
{
	if (!ccli->prompt)
		return;

	echo_str(ccli, ccli->prompt);
}

struct line_buf {
	char *line;
	int size;
	int len;
	int pos;
};

static int line_init(struct line_buf *line)
{
	memset(line, 0, sizeof(*line));
	line->line = calloc(1, BUFSIZ);
	line->size = BUFSIZ;
	if (!line->line)
		return -1;
	return 0;
}

static void line_reset(struct line_buf *line)
{
	memset(line->line, 0, line->size);
	line->len = 0;
	line->pos = 0;
}

static void line_cleanup(struct line_buf *line)
{
	free(line->line);
	memset(line, 0, sizeof(*line));
}

static int line_insert(struct line_buf *line, char ch)
{
	char *extend_line;
	int len;

	if (line->len == line->size) {
		extend_line = realloc(line->line, line->size + BUFSIZ);
		if (!extend_line)
			return -1;
		line->line = extend_line;
		line->size += BUFSIZ;
	}

	if (line->pos < line->len) {
		len = line->len - line->pos;
		memmove(line->line + line->pos + 1, line->line + line->pos, len);
	}

	line->line[line->pos++] = ch;
	line->len++;

	return 0;
}

static void line_right(struct line_buf *line)
{
	if (line->pos < line->len)
		line->pos++;
}

static void line_left(struct line_buf *line)
{
	if (line->pos)
		line->pos--;
}

static void line_backspace(struct line_buf *line)
{
	int len;

	if (!line->pos)
		return;

	len = line->len - line->pos;
	line->pos--;
	line->len--;
	memmove(line->line + line->pos, line->line + line->pos + 1, len);
	line->line[line->len] = '\0';
}

static void line_del(struct line_buf *line)
{
	int len;

	if (line->pos == line->len)
		return;

	len = line->len - line->pos;
	line->len--;
	memmove(line->line + line->pos, line->line + line->pos + 1, len);
	line->line[line->len] = '\0';
}

static int line_copy(struct line_buf *dst, struct line_buf *src, int len)
{
	dst->size = src->size;
	dst->line = calloc(1, src->size);
	if (!dst->line)
		return -1;

	if (len > src->len)
		len = src->len;
	strncpy(dst->line, src->line, len);
	dst->pos = len;
	dst->len = len;

	return 0;
}

static void free_argv(int argc, char **argv)
{
	for (argc--; argc >= 0; argc--)
		free(argv[argc]);
	free(argv);
}

static int line_parse(struct line_buf *line, char ***pargv)
{
	char **argv = NULL;
	char *arg;
	char **v;
	char *p = line->line;
	char *u;
	char *word;
	char q = 0;
	int argc = 0;
	int len;

	while (*p) {
		bool last = false;

		while (isspace(*p))
			p++;

		if (!*p)
			break;

		word = p;

		for ( ; !last && *p; p++) {

			switch (*p) {
			case '\'':
			case '"':
				if (!q)
					q = *p;
				else if (*p == q)
					q = 0;
				break;
			case '\\':
				p++;
				if (!*p)
					p--;
				break;

			default:
				if (q)
					break;
				if (isspace(*p))
					last = true;
				break;
			}
		}

		/* Do not include the space that was found */
		if (last)
			p--;

		len = p - word;

		arg = malloc(len + 1);
		if (!arg)
			goto fail;

		v = realloc(argv, sizeof(*v) * (argc + 1));
		if (!v) {
			free(arg);
			goto fail;
		}
		argv = v;

		strncpy(arg, word, len);
		arg[len] = '\0';

		/* Now remove quotes and backslashes */
		u = word = arg;
		q = 0;
		for (; *u; u++) {
			switch (*u) {
			case '\'':
			case '"':
				if (!q)
					q = *u;
				else if (*u == q)
					q = 0;
				break;
			case '\\':
				u++;
				if (!*u)
					u--;
				/* fallthrough */
			default:
				*word++ = *u;
				break;
			}
		}
		*word = '\0';
		argv[argc++] = arg;
	}

	*pargv = argv;

	return argc;

 fail:
	free_argv(argc, argv);
	return -1;
}

static void line_replace(struct line_buf *line, char *str)
{
	int len = strlen(str);

	if (len >= line->size)
		len = line->size - 1;

	strncpy(line->line, str, len);
	if (len < line->len)
		memset(line->line + len, 0, line->len - len);
	line->len = len;
	line->pos = len;
}

static void clear_line(struct ccli *ccli, struct line_buf *line)
{
	int i;

	echo(ccli, '\r');

	for (i = 0; i < line->len; i++)
		echo(ccli, ' ');
}

static int history_add(struct ccli *ccli, char *line)
{
	char **lines;
	int idx;

	if (ccli->history_size < ccli->history_max) {
		lines = realloc(ccli->history, sizeof(*lines) * ccli->history_size + 1);
		if (!lines)
			return -1;
		lines[ccli->history_size] = NULL;
		ccli->history = lines;
	}

	idx = ccli->history_size % ccli->history_max;

	free(ccli->history[idx]);
	ccli->history[idx] = strdup(line);
	if (!ccli->history[idx])
		return -1;

	ccli->history_size++;
	ccli->current_line = ccli->history_size;

	return 0;
}

static int history_up(struct ccli *ccli, struct line_buf *line)
{
	int idx;

	if (!ccli->current_line ||
	    (ccli->history_size > ccli->history_max &&
	     ccli->current_line <= ccli->history_size - ccli->history_max))
		return 0;

	ccli->current_line--;
	idx = ccli->current_line % ccli->history_max;
	clear_line(ccli, line);
	line_replace(line, ccli->history[idx]);
	return 0;
}

static int history_down(struct ccli *ccli, struct line_buf *line)
{
	int idx;

	if (ccli->current_line >= (ccli->history_size - 1))
		return 0;

	ccli->current_line++;
	idx = ccli->current_line % ccli->history_max;
	clear_line(ccli, line);
	line_replace(line, ccli->history[idx]);
	return 0;
}

static int exec_exit(struct ccli *ccli, const char *command,
		     const char *line, void *data,
		     int argc, char **argv)
{
	printf("Exiting\n");
	return 1;
}

/**
 * ccli_alloc - Allocate a new ccli descriptor.
 * @prompt: The prompt to display (NULL for none)
 * @in: The input file descriptor.
 * @out: The output file descriptor.
 *
 * Allocates the comand line interface descriptor, taking
 * over control of the @in and @out file descriptors.
 *
 * Returns the allocated descriptor on success (must be freed with
 *   ccli_free(), and NULL on error.
 */
struct ccli *ccli_alloc(const char *prompt, int in, int out)
{
	struct termios ttyin;
	struct ccli *ccli;

	ccli = calloc(1, sizeof(*ccli));
	if (!ccli)
		return NULL;

	if (prompt) {
		ccli->prompt = strdup(prompt);
		if (!ccli->prompt)
			goto free;
	}

	ccli->in = in;
	ccli->out = out;

	ccli->history_max = DEFAULT_HISTORY_MAX;

	memset(&savein, 0, sizeof(savein));
	memset(&saveout, 0, sizeof(saveout));
	memset(&ttyin, 0, sizeof(savein));

	tcgetattr(STDIN_FILENO, &savein);
	tcgetattr(STDOUT_FILENO, &saveout);

	atexit(cleanup);

	tcgetattr(in, &ttyin);
	ttyin.c_lflag &= ~ICANON;
	ttyin.c_lflag &= ~(ECHO | ECHONL | ISIG);
	tcsetattr(in, TCSANOW, (void*)&ttyin);

	ccli_register_command(ccli, "exit", exec_exit, NULL);

	return ccli;

 free:
	ccli_free(ccli);
	return NULL;
}

/**
 * ccli_free - Free an allocated ccli descriptor.
 * @ccli: The descriptor to free.
 *
 * Free the @ccli descriptor created with ccli_alloc().
 */
void ccli_free(struct ccli *ccli)
{
	int i;

	if (!ccli)
		return;

	free(ccli->prompt);

	for (i = 0; i < ccli->nr_commands; i++)
		free(ccli->commands[i].cmd);

	free(ccli->commands);
	free(ccli);
}

/**
 * ccli_printf - Write to the output descriptor of ccli
 * @ccli: The CLI descriptor to write to.
 * @fmt: A printf() like format to write.
 *
 * Writes to the output descriptor of @ccli the content passed in.
 *
 * Returns the number of characters written on success, and
 *   -1 on error.
 */
int ccli_printf(struct ccli *ccli, const char *fmt, ...)
{
	va_list ap1, ap2;
	char *buf = NULL;
	int len;

	va_start(ap1, fmt);
	va_copy(ap2, ap1);
	len = vsnprintf(NULL, 0, fmt, ap1);
	va_end(ap1);

	if (len > 0) {
		buf = malloc(len + 1);
		if (buf)
			len = vsnprintf(buf, len + 1, fmt, ap2);
		else
			len = -1;
	}
	va_end(ap2);

	if (len > 0)
		echo_str(ccli, buf);
	free(buf);

	return len;
}

static struct command *find_command(struct ccli *ccli, const char *cmd)
{
	int i;

	for (i = 0; i < ccli->nr_commands; i++) {
		if (strcmp(cmd, ccli->commands[i].cmd) == 0)
			return &ccli->commands[i];
	}

	return NULL;
}

/**
 * ccli_register_command - Register a command for the CLI interface.
 * @ccli: The CLI descriptor to register a command for.
 * @command_name: the text that will execute the command.
 * @callback: The callback function to call when the command is executed.
 * @data: Data to pass to the @callback function.
 *
 * Register the command @command_name to @ccli. When the user types
 * that command, it will trigger the @callback which will be passed
 * the arguments that the user typed, along with the @data pointer.
 *
 * Returns 0 on success and -1 on error.
 */
int ccli_register_command(struct ccli *ccli, const char *command_name,
			  ccli_command_callback callback, void *data)
{
	struct command *commands;
	char *cmd;

	if (!ccli || !command_name || !callback) {
		errno = -EINVAL;
		return -1;
	}

	commands = find_command(ccli, command_name);
	if (commands) {
		/* override it with this one */
		commands->callback = callback;
		commands->data = data;
		return 0;
	}

	cmd = strdup(command_name);
	if (!cmd)
		return -1;

	commands = realloc(ccli->commands,
			   sizeof(*commands) * (ccli->nr_commands + 1));
	if (!commands) {
		free(cmd);
		return -1;
	}

	commands[ccli->nr_commands].cmd = cmd;
	commands[ccli->nr_commands].callback = callback;
	commands[ccli->nr_commands].data = data;

	ccli->commands = commands;
	ccli->nr_commands++;

	return 0;
}

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

static int execute(struct ccli *ccli, struct line_buf *line)
{
	struct command *cmd;
	char **argv;
	int argc;
	int ret = 0;

	argc = line_parse(line, &argv);
	if (argc < 0) {
		echo_str(ccli, "Error parsing command\n");
		return 0;
	}

	if (!argc)
		return 0;

	cmd = find_command(ccli, argv[0]);

	if (cmd) {
		ret = cmd->callback(ccli, cmd->cmd,
				    line->line, cmd->data,
				    argc, argv);
	} else {
		echo_str(ccli, "Command not found: ");
		echo_str(ccli, argv[0]);
		echo(ccli, '\n');
	}

	free_argv(argc, argv);

	history_add(ccli, line->line);

	return ret;
}

static void refresh(struct ccli *ccli, struct line_buf *line)
{
	int len;

	echo(ccli, '\r');

	echo_prompt(ccli);
	echo_str(ccli, line->line);
	echo_str(ccli, "  \b\b");

	for (len = line->len; len > line->pos; len--)
		echo(ccli, '\b');
}

static void insert_word(struct ccli *ccli, struct line_buf *line,
			const char *word, int len)
{
	int i;

	for (i = 0; i < len; i++)
		line_insert(line, word[i]);
	line_insert(line, ' ');
	refresh(ccli, line);
}

static void word_completion(struct ccli *ccli, struct line_buf *line, int tab)
{
	struct command *cmd;
	struct line_buf copy;
	char **list = NULL;
	char **argv;
	char *match;
	int matched = 0;
	int word;
	int argc;
	int mlen;
	int len;
	int cnt = 0;
	int ret;
	int m;
	int i;

	ret = line_copy(&copy, line, line->pos);
	if (ret < 0)
		return;

	argc = line_parse(&copy, &argv);
	if (argc <= 0)
		goto out;

	word = argc - 1;

	/* If the cursor is on a space, there's no word to match */
	if (isspace(copy.line[copy.pos - 1])) {
		match = "";
		word++;
	} else {
		match = argv[word];
	}

	cmd = find_command(ccli, argv[0]);
	if (cmd && cmd->completion)
		cnt = cmd->completion(ccli, cmd->cmd, copy.line, word,
				      match, &list, cmd->data);

	mlen = strlen(match);

	if (cnt) {
		for (i = 0; i < cnt; i++) {
			/* If list[i] failed to allocate, we need to handle that */
			if (!list[i])
				continue;
			if (!mlen || strncmp(list[i], match, mlen) == 0) {
				matched++;
				m = i;
			}
		}

		if (matched == 1) {
			len = strlen(list[m]);
			insert_word(ccli, line, list[m] + mlen, len - mlen);

		} else if (tab && matched > 1) {
			echo(ccli, '\n');

			for (i = 0; i < cnt; i++) {
				/* If list[i] failed to allocate, we need to handle that */
				if (!list[i])
					continue;
				if (!mlen || strncmp(list[i], match, mlen) == 0) {
					if (i)
						echo(ccli, ' ');
					echo_str(ccli, list[i]);
				}
			}
			echo(ccli, '\n');
			refresh(ccli, line);
		}

		for (i = 0; i < cnt; i++)
			free(list[i]);
		free(list);
	}

	free_argv(argc, argv);
 out:
	line_cleanup(&copy);
}

static void do_completion(struct ccli *ccli, struct line_buf *line, int tab)
{
	struct command *command;
	int len;
	int i = line->pos - 1;
	int s, m = 0;
	int match = -1;

	/* Completion currently only works with the first word */
	while (i >= 0 && !isspace(line->line[i]))
		i--;

	s = i + 1;

	while (i >= 0 && isspace(line->line[i]))
		i--;

	/* If the pos was at the first word, i will be less than zero */
	if (i >= 0)
		return word_completion(ccli, line, tab);

	len = line->pos - s;

	/* Find how many commands match */
	for (i = 0; i < ccli->nr_commands; i++) {
		command = &ccli->commands[i];
		if (!len || strncmp(line->line + s, command->cmd, len) == 0) {
			match = i;
			m++;
		}
	}

	if (!m)
		return;

	if (m == 1) {
		/* select it */
		command = &ccli->commands[match];
		m = strlen(command->cmd);
		insert_word(ccli, line, command->cmd + len, m - len);
		return;
	}

	/* list all the matches if tab was hit more than once */
	if (!tab)
		return;

	echo(ccli, '\n');

	for (i = 0; i < ccli->nr_commands; i++) {
		command = &ccli->commands[i];
		if (!len || strncmp(line->line + s, command->cmd, len) == 0) {
			if (i)
				echo(ccli, ' ');
			echo_str(ccli, command->cmd);
		}
	}
	echo(ccli, '\n');
	refresh(ccli, line);
}

/**
 * ccli_loop - Execute a command loop for the user.
 * @ccli: The CLI descriptor to execute on.
 *
 * This reads the input descriptor of @ccli and lets the user
 * execute shell like commands on the command line.
 *
 * Returns 0 on success (when one of the commands exits the loop)
 *  or -1 on error.
 */
int ccli_loop(struct ccli *ccli)
{
	struct line_buf line;
	bool bracket = false;
	bool three = false;
	bool esc = false;
	unsigned char ch;
	int ret = 0;
	int tab = 0;
	int r;

	if (line_init(&line))
		return -1;

	echo_prompt(ccli);

	while (!ret) {
		r = read(ccli->in, &ch, 1);
		if (r <= 0)
			break;

		if (ch != '\t')
			tab = 0;
		switch (ch) {
		case '\n':
			echo(ccli, '\n');
			ret = execute(ccli, &line);
			if (ret)
				break;
			line_reset(&line);
			echo_prompt(ccli);
			break;
		case '\t':
			do_completion(ccli, &line, tab++);
			break;
		case 3: /* ETX */
			echo_str(ccli, "^C\n");
			ret = 1;
			break;
		case 27: /* ESC */
			esc = true;
			break;
		case 127: /* DEL */
			line_backspace(&line);
			refresh(ccli, &line);
			break;
		default:
			if (esc) {
				esc = false;
				if (ch != '[') {
					dprint("unknown esc char %c (%d)\n", ch, ch);
					break;
				}
				bracket = true;
				break;
			}
			if (bracket) {
				bracket = false;
				switch (ch) {
				case 'A':
					history_up(ccli, &line);
					refresh(ccli, &line);
					break;
				case 'B':
					history_down(ccli, &line);
					refresh(ccli, &line);
					break;
				case 'C':
					line_right(&line);
					refresh(ccli, &line);
					break;
				case 'D':
					line_left(&line);
					refresh(ccli, &line);
					break;
				case '3':
					three = true;
					break;
				default:
					dprint("unknown bracket %c (%d)\n", ch, ch);
					break;
				}
				break;
			}
			if (three) {
				three = false;
				switch (ch) {
				case '~':
					line_del(&line);
					refresh(ccli, &line);
					break;
				default:
					dprint("unknown 'three' %c (%d)\n", ch, ch);
					break;
				}
				break;
			}
			if (isprint(ch)) {
				line_insert(&line, ch);
				refresh(ccli, &line);
				break;
			}
			dprint("unknown char '%d'\n", ch);
		}
	}

	line_cleanup(&line);
	return 0;
}
