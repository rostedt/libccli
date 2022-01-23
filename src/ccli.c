// SPDX-License-Identifier: LGPL-2.1
/*
 * CLI interface for C.
 *
 * Copyright (C) 2022 Steven Rostedt <rostedt@goodmis.org>
 */
#include <stdarg.h>
#include <termios.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>

#include "ccli-local.h"

//#define DEBUG
#ifdef DEBUG
#define dprint printf
#else
#define dprint(x...)
#endif

#define DEFAULT_HISTORY_MAX	256
#define DEFAULT_PAGE_SCROLL	24

struct command {
	char			*cmd;
	ccli_command_callback	callback;
	ccli_completion		completion;
	void			*data;
};

struct ccli {
	struct termios		savein;
	struct termios		saveout;
	struct line_buf		*line;
	char			*temp_line;
	int			history_max;
	int			history_size;
	int			current_line;
	int			in;
	int			out;
	int			nr_commands;
	struct command		*commands;
	struct command		enter;
	struct command		unknown;
	ccli_interrupt		interrupt;
	void			*interrupt_data;
	char			*prompt;
	char			**history;
};

static void cleanup(struct ccli *ccli)
{
	tcsetattr(ccli->in, TCSANOW, &ccli->savein);
	tcsetattr(ccli->out, TCSANOW, &ccli->saveout);
}

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

static void clear_line(struct ccli *ccli, struct line_buf *line)
{
	int len;
	int i;

	echo(ccli, '\r');

	len = line->len;
	if (ccli->prompt)
		len += strlen(ccli->prompt);

	for (i = 0; i < len; i++)
		echo(ccli, ' ');
}

static int history_add(struct ccli *ccli, char *line)
{
	char **lines;
	int idx;

	if (ccli->history_size < ccli->history_max) {
		lines = realloc(ccli->history, sizeof(*lines) * (ccli->history_size + 1));
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

static int history_up(struct ccli *ccli, struct line_buf *line, int cnt)
{
	int current = ccli->current_line;
	int idx;
	char *str;

	if (ccli->current_line > cnt)
		ccli->current_line -= cnt;
	else
		ccli->current_line = 0;

	if (ccli->history_size > ccli->history_max &&
	    ccli->current_line <= ccli->history_size - ccli->history_max)
		ccli->current_line = (ccli->history_size - ccli->history_max) + 1;

	if (current == ccli->current_line)
		return 1;

	clear_line(ccli, line);

	/* Store the current line in case it was modifed */
	str = strdup(ccli->line->line);
	if (str) {
		if (current >= ccli->history_size) {
			free(ccli->temp_line);
			ccli->temp_line = str;
		} else {
			idx = current % ccli->history_max;
			free(ccli->history[idx]);
			ccli->history[idx] = str;
		}
	}

	idx = ccli->current_line % ccli->history_max;
	line_replace(line, ccli->history[idx]);
	return 0;
}

static int history_down(struct ccli *ccli, struct line_buf *line, int cnt)
{
	int current = ccli->current_line;
	int idx;
	char *str;

	ccli->current_line += cnt;

	if (ccli->current_line > (ccli->history_size - 1))
		ccli->current_line = ccli->history_size - 1;

	if (current == ccli->current_line) {
		/* Restore the command that was before moving in history */
		if (ccli->temp_line) {
			clear_line(ccli, line);
			line_replace(line, ccli->temp_line);
			free(ccli->temp_line);
			ccli->temp_line = NULL;
		}
		return 1;
	}

	clear_line(ccli, line);

	/* Store the current line in case it was modifed */
	str = strdup(ccli->line->line);
	if (str) {
		idx = current % ccli->history_max;
		free(ccli->history[idx]);
		ccli->history[idx] = str;
	}

	idx = ccli->current_line % ccli->history_max;
	line_replace(line, ccli->history[idx]);
	return 0;
}

static int unknown_default(struct ccli *ccli, const char *command,
			   const char *line, void *data,
			   int argc, char **argv)
{
	echo_str(ccli, "Command not found: ");
	echo_str(ccli, argv[0]);
	echo(ccli, '\n');
	return 0;
}

static int enter_default(struct ccli *ccli, const char *command,
			 const char *line, void *data,
			 int argc, char **argv)
{
	return 0;
}

static int exec_exit(struct ccli *ccli, const char *command,
		     const char *line, void *data,
		     int argc, char **argv)
{
	ccli_printf(ccli, "Exiting\n");
	return 1;
}

static int interrupt_default(struct ccli *ccli, const char *line,
			     int pos, void *data)
{
	echo_str(ccli, "^C\n");
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

	memset(&ccli->savein, 0, sizeof(ccli->savein));
	memset(&ccli->saveout, 0, sizeof(ccli->saveout));
	memset(&ttyin, 0, sizeof(ccli->savein));

	tcgetattr(STDIN_FILENO, &ccli->savein);
	tcgetattr(STDOUT_FILENO, &ccli->saveout);

	tcgetattr(in, &ttyin);
	ttyin.c_lflag &= ~ICANON;
	ttyin.c_lflag &= ~(ECHO | ECHONL | ISIG);
	tcsetattr(in, TCSANOW, (void*)&ttyin);

	ccli_register_command(ccli, "exit", exec_exit, NULL);
	ccli->unknown.callback = unknown_default;
	ccli->enter.callback = enter_default;
	ccli->interrupt = interrupt_default;

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

	cleanup(ccli);

	free(ccli->prompt);

	for (i = 0; i < ccli->nr_commands; i++)
		free(ccli->commands[i].cmd);

	for (i = 0; i < ccli->history_size; i++)
		free(ccli->history[i]);
	free(ccli->history);

	free(ccli->commands);
	free(ccli->temp_line);
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
 * ccli_unregister_command - Remove a command from the CLI interface
 * @ccli: The CLI descriptor to remove the command from
 * @command_name: The command name to remove
 *
 * Removes the registered command named @command_name from @ccli.
 *
 * Returns 0 on successful removal, or -1 on error or @command_name not found.
 *   ERRNO will only be set for error and will not be touched if the
 *  @command_name was not found.
 */
int ccli_unregister_command(struct ccli *ccli, const char *command_name)
{
	struct command *commands;
	int cnt;

	if (!ccli || !command_name) {
		errno = -EINVAL;
		return -1;
	}

	commands = find_command(ccli, command_name);
	if (!commands)
		return -1;

	free(commands->cmd);

	cnt = (ccli->nr_commands - (commands - ccli->commands)) - 1;
	if (cnt)
		memmove(commands, commands + 1, cnt * sizeof(*commands));
	ccli->nr_commands--;
	commands = &ccli->commands[ccli->nr_commands];
	memset(commands, 0, sizeof(*commands));

	return 0;
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
 * ccli_register_default - Register a callback for just "enter".
 * @ccli: The CLI descriptor to register the default for.
 * @callback: The callback function to call when just "enter" is hit.
 * @data: Data to pass to the @callback function.
 *
 * When the user hits enter with nothing on the command line, the
 * @callback function will be called just like any other command is
 * called, but the command_name parameter will be empty.
 *
 * Returns 0 on success and -1 on error.
 */
int ccli_register_default(struct ccli *ccli,
			  ccli_command_callback callback, void *data)
{
	if (!ccli || !callback) {
		errno = -EINVAL;
		return -1;
	}

	ccli->enter.callback = callback;
	ccli->enter.data = data;

	return 0;
}

/**
 * ccli_register_unknown - Register a callback for unknown commands.
 * @ccli: The CLI descriptor to register the default for.
 * @callback: The callback function to call when a command is unknown
 * @data: Data to pass to the @callback function.
 *
 * When the user hits enters a command that is not registered, the
 * @callback function will be called just like any other command is
 * called.
 *
 * Returns 0 on success and -1 on error.
 */
int ccli_register_unknown(struct ccli *ccli,
			  ccli_command_callback callback, void *data)
{
	if (!ccli || !callback) {
		errno = -EINVAL;
		return -1;
	}

	ccli->unknown.callback = callback;
	ccli->unknown.data = data;

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

/**
 * ccli_register_interrupt - Register what to do on SIGINT
 * @ccli: The CLI descriptor to register to.
 * @callback: The callback function to call when user hits Ctrl^C
 * @data: The data to pass to the callback.
 *
 * This allows the application to register a function when the user
 * hits Ctrl^C. Currently the default operation is simply to print
 * "^C\n" and exit. But this allows the application to override that
 * operation.
 *
 * Returns 0 on success and -1 on error.
 */
int ccli_register_interrupt(struct ccli *ccli, ccli_interrupt callback,
			    void *data)
{
	if (!ccli || !callback) {
		errno = -EINVAL;
		return -1;
	}

	ccli->interrupt = callback;
	ccli->interrupt_data = data;
	return 0;
}

/**
 * ccli_history - return a previous entered line from the past
 * @ccli: The ccli descriptor to read the histor from
 * @past: How far back to go
 *
 * Reads the line that happened @past commands ago and returns it.
 *
 * Returns a string that should not be modifiied if there was
 *   a command that happened @past commands ago, otherwise NULL.
 */
const char *ccli_history(struct ccli *ccli, int past)
{
	int idx;

	if (past > ccli->history_size ||
	    past > ccli->history_max)
		return NULL;

	idx = ccli->history_size - past;
	idx %= ccli->history_max;

	return ccli->history[idx];
}

/**
 * ccli_line_clear - clear the internal line
 * ccli: The CLI desciptor to clear the command line for.
 *
 * This will clear out the internal contents stored for the command.
 * Note, it does not affect what the user is displayed.
 * This is useful for the interrupt callback to clear the line if
 * need be.
 */
void ccli_line_clear(struct ccli *ccli)
{
	if (!ccli || !ccli->line)
		return;

	line_reset(ccli->line);
}

/**
 * ccli_line_inject - Inject content into the command line at pos.
 * @ccli: The CLI descriptor to inject content into.
 * @str: The string to inject.
 * @pos: The position to inject the line into.
 *
 * This injects content into the internal command line at @pos. If @pos
 * is negative, then it just injects @str at the current location
 * of line->pos. If @pos is greater than the length of line->line,
 * then it will simply append the string to the line.
 *
 * Returns 0 on success or -1 on failure.
 */
int ccli_line_inject(struct ccli *ccli, const char *str, int pos)
{
	struct line_buf *line;
	int ret = 0;
	int i;

	if (!ccli || !ccli->line || !str) {
		errno = -EINVAL;
		return -1;
	}

	line = ccli->line;

	if (pos >= 0)
		line->pos = pos > line->len ? line->len : pos;

	for (i = 0; i < strlen(str); i++)
		ret |= line_insert(line, str[i]);

	return ret;
}

static int execute(struct ccli *ccli, struct line_buf *line, bool hist)
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
		return ccli->enter.callback(ccli, "", line->line,
					    ccli->enter.data,
					    0, NULL);

	cmd = find_command(ccli, argv[0]);

	if (cmd) {
		ret = cmd->callback(ccli, cmd->cmd,
				    line->line, cmd->data,
				    argc, argv);
	} else {
		ret = ccli->unknown.callback(ccli, argv[0], line->line,
					     ccli->unknown.data,
					     argc, argv);
	}

	free_argv(argc, argv);

	if (hist)
		history_add(ccli, line->line);

	return ret;
}

/**
 * ccli_execute - Execute a command from outside the loop
 * @ccli: The ccli descriptor
 * @line_str: The line to execute as if a user typed it.
 * @hist: Add to history or not?
 *
 * Execute a line as if the user typed it from outside the loop.
 * The application may want to initiate some commands before executing
 * the loop (like to reply commands from a previous session). This
 * gives that ability.
 *
 * If @hist is true, then the command is added to the history,
 * otherwise it is not.
 *
 * Returns whatever the command being executed would return.
 *  If a memory allocation happens, -1 is returned and ERRNO is set.
 */
int ccli_execute(struct ccli *ccli, const char *line_str, bool hist)
{
	struct line_buf *old_line = ccli->line;
	struct line_buf line;
	int ret;

	ret = line_init_str(&line, line_str);
	if (ret < 0)
		return ret;

	ccli->line = &line;
	ret = execute(ccli, &line, hist);
	line_cleanup(&line);
	ccli->line = old_line;
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

/**
 * ccli_line_refresh: Refresh the displayed line.
 * @ccli: The CLI descriptor to refresh the command line for.
 *
 * In case the interrupt callback makes modifications to the command line,
 * it can call this to refresh it to show the conents of the internal
 * line.
 */
void ccli_line_refresh(struct ccli *ccli)
{
	if (!ccli || !ccli->line)
		return;

	refresh(ccli, ccli->line);
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
	if (ISSPACE(copy.line[copy.pos - 1])) {
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
	while (i >= 0 && !ISSPACE(line->line[i]))
		i--;

	s = i + 1;

	while (i >= 0 && ISSPACE(line->line[i]))
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
	bool esc = false;
	unsigned char ch;
	int num = 0;
	int ret = 0;
	int tab = 0;
	int r;

	if (line_init(&line))
		return -1;

	ccli->line = &line;

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
			ret = execute(ccli, &line, true);
			if (ret)
				break;
			line_reset(&line);
			echo_prompt(ccli);
			break;
		case '\t':
			do_completion(ccli, &line, tab++);
			break;
		case 3: /* ETX */
			ret = ccli->interrupt(ccli, line.line,
					      line.pos, ccli->interrupt_data);
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
					history_up(ccli, &line, 1);
					refresh(ccli, &line);
					break;
				case 'B':
					history_down(ccli, &line, 1);
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
				case '1':
				case '3':
				case '4':
				case '5':
				case '6':
					num = ch - '0';
					break;
				default:
					dprint("unknown bracket %c (%d)\n", ch, ch);
					break;
				}
				break;
			}

			if (num) {
				if (ch != '~') {
					dprint("unknown num=%d %c (%d)\n", num, ch, ch);
					num = 0;
					break;
				}
				switch (num) {
				case 1:
					line_home(&line);
					refresh(ccli, &line);
					break;
				case 3:
					line_del(&line);
					refresh(ccli, &line);
					break;
				case 4:
					line_end(&line);
					refresh(ccli, &line);
					break;
				case 5:
					history_up(ccli, &line, DEFAULT_PAGE_SCROLL);
					refresh(ccli, &line);
					break;
				case 6:
					history_down(ccli, &line, DEFAULT_PAGE_SCROLL);
					refresh(ccli, &line);
					break;
				default:
					break;
				}
				num = 0;
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
	ccli->line = NULL;
	return 0;
}
