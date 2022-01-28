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
#include <sys/ioctl.h>
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

enum {
	CHAR_ERROR		= -1,
	CHAR_BACKSPACE		= -2,
	CHAR_DEL		= -3,
	CHAR_INTR		= -4,
	CHAR_UP			= -5,
	CHAR_DOWN		= -6,
	CHAR_RIGHT		= -7,
	CHAR_LEFT		= -8,
	CHAR_HOME		= -9,
	CHAR_END		= -10,
	CHAR_PAGEUP		= -11,
	CHAR_PAGEDOWN		= -12,
	CHAR_DELWORD		= -13,
	CHAR_RIGHT_WORD		= -14,
	CHAR_LEFT_WORD		= -15,
};

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
	bool			in_tty;
	int			in;
	int			out;
	int			w_row;
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

static void echo_str_len(struct ccli *ccli, char *str, int len)
{
	write(ccli->out, str, len);
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

static int read_char(struct ccli *ccli)
{
	bool bracket = false;
	bool semi = false;
	bool five = false;
	bool esc = false;
	unsigned char ch;
	int num = 0;
	int r;

	for (;;) {
		r = read(ccli->in, &ch, 1);
		if (r <= 0)
			return CHAR_ERROR;

		switch (ch) {
		case 3: /* ETX */
			return CHAR_INTR;
		case 27: /* ESC */
			esc = true;
			break;
		case 127: /* DEL */
			if (esc)
				return CHAR_DELWORD;
			return CHAR_BACKSPACE;
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
					return CHAR_UP;
				case 'B':
					return CHAR_DOWN;
				case 'C':
					return CHAR_RIGHT;
				case 'D':
					return CHAR_LEFT;
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
				switch (ch) {
				case '~':
					break;
				case ';':
					num = 0;
					semi = true;
					break;
				default:
					dprint("unknown num=%d %c (%d)\n", num, ch, ch);
					num = 0;
					break;
				}
				switch (num) {
				case 1:
					return CHAR_HOME;
				case 3:
					return CHAR_DEL;
				case 4:
					return CHAR_END;
				case 5:
					return CHAR_PAGEUP;
				case 6:
					return CHAR_PAGEDOWN;
				default:
					break;
				}
				num = 0;
				break;
			}
			if (semi) {
				if (ch != '5') {
					dprint("Unknown semi %c (%d)\n", ch, ch);
					break;
				}
				semi = false;
				five = true;
				break;
			}
			if (five){
				switch (ch) {
				case 'C':
					return CHAR_RIGHT_WORD;
				case 'D':
					return CHAR_LEFT_WORD;
				default:
					dprint("Unknown five %c (%d)\n", ch, ch);
				}
				five = false;
			}

			if (isprint(ch) || isspace(ch))
				return ch;

			dprint("unknown char '%d'\n", ch);
			return ch;
		}
	}
}

/**
 * ccli_getchar - read a character from ccli stdin
 * @ccli: The command line descriptor to read from
 *
 * Reads a character from ccli->in. Note, it will only return printable
 * characters, -1 on error or EOF and zero on Ctrl^C.
 *
 * Returns the printable input from the user or 0 on Ctrl^C or -1
 *   on error or EOL.
 */
int ccli_getchar(struct ccli *ccli)
{
	int r;

	for (;;) {
		r = read_char(ccli);
		switch (r) {
		case CHAR_INTR:
			return 0;
		case CHAR_BACKSPACE:
		case CHAR_DEL:
		case CHAR_DELWORD:
		case CHAR_UP:
		case CHAR_DOWN:
		case CHAR_RIGHT:
		case CHAR_LEFT:
		case CHAR_HOME:
		case CHAR_END:
		case CHAR_PAGEUP:
		case CHAR_PAGEDOWN:
		case CHAR_RIGHT_WORD:
		case CHAR_LEFT_WORD:
			continue;
		default:
			if (r < 0)
				return -1;
		}
		break;
	}

	return r;
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

	if (ccli->current_line > (ccli->history_size))
		ccli->current_line = ccli->history_size;

	if (ccli->current_line == ccli->history_size) {
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
	ccli->in_tty = isatty(in);

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
 * ccli_in - return the input file descriptor.
 * @ccli: The CLI descriptor to get the input file descriptor from.
 *
 * Returns the file descriptor for @ccli that was given as the
 *   input in ccli_alloc();
 */
int ccli_in(struct ccli *ccli)
{
	return ccli->in;
}

/**
 * ccli_out - return the output file descriptor.
 * @ccli: The CLI descriptor to get the output file descriptor from.
 *
 * Returns the file descriptor for @ccli that was given as the
 *   output in ccli_alloc();
 */
int ccli_out(struct ccli *ccli)
{
	return ccli->out;
}

/**
 * ccli_vprintf - Write to the output descriptor of ccli
 * @ccli: The CLI descriptor to write to.
 * @fmt: A printf() like format to write.
 * @ap: The argument list
 *
 * Writes to the output descriptor of @ccli the content passed in.
 *
 * Returns the number of characters written on success, and
 *   -1 on error.
 */
int ccli_vprintf(struct ccli *ccli, const char *fmt, va_list ap)
{
	va_list ap2;
	char *buf = NULL;
	int len;

	va_copy(ap2, ap);
	len = vsnprintf(NULL, 0, fmt, ap);

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
	va_list ap;
	int len;

	va_start(ap, fmt);
	len = ccli_vprintf(ccli, fmt, ap);
	va_end(ap);

	return len;
}

static char page_stop(struct ccli *ccli)
{
	char ans;

	echo_str(ccli, "--Type <RET> for more, q to quit, c to continue without paging--");
	read(ccli->in, &ans, 1);
	echo(ccli, '\n');
	return ans;
}

/**
 * ccli_page - Write to the output descriptor of ccli and prompt at window size
 * @ccli: The CLI descriptor to write to.
 * @line: The current line count
 * @fmt: A printf() like format to write.
 *
 * Just like ccli_printf() which rites to the output descriptor of @ccli
 * the content passed in, but this will also stop to ask the user to
 * read more, quit or continue when the amount reaches the size of
 * the window.
 *
 * When @line matches the window size, the prompt will be displayed.
 * Note, the window size is only calculated when @line is zero.
 *
 * If @line is less than zero, no prompt will be displayed.
 *
 * Return -1 on error (with ERRNO set) or if the user asks to quit.
 *         1 for another screen full.
 *         0 to not stop.

 */
int ccli_page(struct ccli *ccli, int line, const char *fmt, ...)
{
	struct winsize w;
	va_list ap;
	char ans;
	int len;
	int ret;

	switch (line) {
	case 0:
		break;
	case 1:
		ret = ioctl(ccli->in, TIOCGWINSZ, &w);
		if (ret < 0) {
			line = 0;
			break;
		}
		ccli->w_row = w.ws_row;
		break;
	default:
		if (line < 0)
			return line;
		if (!(line % ccli->w_row)) {
			ans = page_stop(ccli);
			switch (ans) {
			case 'q':
				return -1;
			case 'c':
				line = 0;
				break;
			}
		}
	}

	va_start(ap, fmt);
	len = ccli_vprintf(ccli, fmt, ap);
	va_end(ap);

	return len >= 0 ? line > 0 ? line + 1 : line : len;
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

static void refresh(struct ccli *ccli, struct line_buf *line, int pad)
{
	char padding[pad + 3];
	int len;

	/* Just append two spaces */
	pad += 2;

	memset(padding, ' ', pad);
	padding[pad] = '\0';

	echo(ccli, '\r');

	echo_prompt(ccli);
	echo_str(ccli, line->line);
	echo_str(ccli, padding);
	while (pad--)
		echo(ccli, '\b');

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

	refresh(ccli, ccli->line, 0);
}

static void insert_word(struct ccli *ccli, struct line_buf *line,
			const char *word, int len)
{
	int i;

	for (i = 0; i < len; i++)
		line_insert(line, word[i]);
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
		refresh(ccli, line, 0);

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
		refresh(ccli, line, 0);
		goto out_free;
	}

	/* list all the matches if tab was hit more than once */
	if (!tab)
		goto out_free;

	echo(ccli, '\n');

	print_completion(ccli, line->line + s, len,
			  ccli->nr_commands, commands);

	refresh(ccli, line, 0);
 out_free:
	free(commands);
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
	char ch;
	int tab = 0;
	int ret = 0;
	int pad;

	if (line_init(&line))
		return -1;

	ccli->line = &line;

	echo_prompt(ccli);

	while (!ret) {
		ch = read_char(ccli);
		if (ch == CHAR_ERROR)
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
		case CHAR_INTR:
			ret = ccli->interrupt(ccli, line.line,
					      line.pos, ccli->interrupt_data);
			break;
		case CHAR_BACKSPACE:
			line_backspace(&line);
			refresh(ccli, &line, 0);
			break;
		case CHAR_DEL:
			line_del(&line);
			refresh(ccli, &line, 0);
			break;
		case CHAR_DELWORD:
			pad = line_del_word(&line);
			refresh(ccli, &line, pad);
			break;
		case CHAR_UP:
			history_up(ccli, &line, 1);
			refresh(ccli, &line, 0);
			break;
		case CHAR_DOWN:
			history_down(ccli, &line, 1);
			refresh(ccli, &line, 0);
			break;
		case CHAR_LEFT:
			line_left(&line);
			refresh(ccli, &line, 0);
			break;
		case CHAR_RIGHT:
			line_right(&line);
			refresh(ccli, &line, 0);
			break;
		case CHAR_HOME:
			line_home(&line);
			refresh(ccli, &line, 0);
			break;
		case CHAR_END:
			line_end(&line);
			refresh(ccli, &line, 0);
			break;
		case CHAR_PAGEUP:
			history_up(ccli, &line, DEFAULT_PAGE_SCROLL);
			refresh(ccli, &line, 0);
			break;
		case CHAR_PAGEDOWN:
			history_down(ccli, &line, DEFAULT_PAGE_SCROLL);
			refresh(ccli, &line, 0);
			break;
		case CHAR_LEFT_WORD:
			line_left_word(&line);
			refresh(ccli, &line, 0);
			break;
		case CHAR_RIGHT_WORD:
			line_right_word(&line);
			refresh(ccli, &line, 0);
			break;

		default:
			if (isprint(ch)) {
				line_insert(&line, ch);
				refresh(ccli, &line, 0);
				break;
			}
			dprint("unknown char '%d'\n", ch);
		}
	}

	line_cleanup(&line);
	ccli->line = NULL;
	return 0;
}
