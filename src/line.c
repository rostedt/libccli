// SPDX-License-Identifier: MIT
/*
 * Copyright (C) 2022 Steven Rostedt <rostedt@goodmis.org>
 */
#include "ccli-local.h"

__hidden void free_argv(int argc, char **argv)
{
	for (argc--; argc >= 0; argc--)
		free(argv[argc]);
	free(argv);
}

__hidden int line_init(struct line_buf *line)
{
	memset(line, 0, sizeof(*line));
	line->line = calloc(1, BUFSIZ);
	line->size = BUFSIZ;
	if (!line->line)
		return -1;
	return 0;
}

/**
 * line_init_str - Initialize a line_buf with a given string
 * @line: The line_buf to initialize
 * @str: The string to initialize it with.
 *
 * Initilize the line with a given string.
 *
 * Returns 0 on success and -1 on failure.
 */
__hidden int line_init_str(struct line_buf *line, const char *str)
{
	int len = strlen(str);
	/* strlen(str) + 1 + BUFSIZ - 1 */
	int size = ((len + BUFSIZ) / BUFSIZ) * BUFSIZ;

	memset(line, 0, sizeof(*line));
	line->line = calloc(1, size);
	line->size = size;
	line->len = len;
	line->pos = len;

	if (!line->line)
		return -1;

	strcpy(line->line, str);

	return 0;
}

__hidden void line_reset(struct line_buf *line)
{
	memset(line->line, 0, line->size);
	line->len = 0;
	line->pos = 0;
	line->start = 0;
}

__hidden void line_cleanup(struct line_buf *line)
{
	free(line->line);
	memset(line, 0, sizeof(*line));
}

__hidden int line_insert(struct line_buf *line, char ch)
{
	char *extend_line;
	int len;

	/*
	 * In the case of '\' followed by a newline, the '\'
	 * should no longer be part of the line string (although
	 * it stays printed). And the "start" of the line becomes
	 * the current position.
	 */
	if (ch == CHAR_NEWLINE) {
		/* start a new line */
		if (!line->len || line->line[line->len - 1] != '\\') {
			fprintf(stderr, "Bad new line escape?\n");
			return -1;
		}
		line->len--;
		line->pos = line->len;
		line->line[line->len] = '\0';
		line->start = line->len;
		return 0;
	}
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

__hidden bool line_state_escaped(struct line_buf *line)
{
	int escape = 0;
	int i;

	for (i = line->len - 1; i >= 0 && line->line[i] == '\\'; i--)
		escape ^= 1;

	return escape;
}

__hidden void line_right(struct line_buf *line)
{
	if (line->pos < line->len)
		line->pos++;
}

__hidden void line_left(struct line_buf *line)
{
	if (line->pos > line->start)
		line->pos--;
}

__hidden void line_home(struct line_buf *line)
{
	line->pos = line->start;
}

__hidden void line_end(struct line_buf *line)
{
	line->pos = line->len;
}

__hidden void line_backspace(struct line_buf *line)
{
	int len;

	if (line->pos == line->start)
		return;

	len = line->len - line->pos;
	line->pos--;
	line->len--;
	memmove(line->line + line->pos, line->line + line->pos + 1, len);
	line->line[line->len] = '\0';
}

__hidden void line_right_word(struct line_buf *line)
{
	while (line->pos < line->len && !isalnum(line->line[++line->pos]))
		;

	while (line->pos < line->len && isalnum(line->line[++line->pos]))
		;
}

__hidden void line_left_word(struct line_buf *line)
{
	while ((line->pos-- > line->start) && !isalnum(line->line[line->pos]))
		;

	while ((line->pos > line->start) && isalnum(line->line[line->pos]))
		line->pos--;

	if (!isalnum(line->line[line->pos]))
		line->pos++;
}

static int del_words(struct line_buf *line, int old_pos)
{
	int len;

	len = line->len - old_pos;
	line->len -= old_pos - line->pos;
	memmove(line->line + line->pos, line->line + old_pos, len);
	memset(line->line + line->len, 0, old_pos - line->pos);

	return old_pos - line->pos;
}

__hidden int line_del_beginning(struct line_buf *line)
{
	int s = line->pos;

	if (line->pos == line->start)
		return 0;

	line_home(line);
	return del_words(line, s);
}

__hidden int line_del_word(struct line_buf *line)
{
	int s = line->pos;

	if (line->pos == line->start)
		return 0;

	line_left_word(line);
	return del_words(line, s);
}

__hidden void line_del(struct line_buf *line)
{
	int len;

	if (line->pos == line->len)
		return;

	len = line->len - line->pos;
	line->len--;
	memmove(line->line + line->pos, line->line + line->pos + 1, len);
	line->line[line->len] = '\0';
}

__hidden int line_copy(struct line_buf *dst, struct line_buf *src, int len)
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

static bool match_delim(const char *word, const char *delim, int dlen)
{
	return delim && strncmp(word, delim, dlen) == 0;
}

/**
 * ccli_line_parse_multi - parse a string into its arguments
 * @line: The string to parse
 * @pargv: A pointer to place the array of strings
 * @delim: A delimiter to end the current parsing (unlessed escaped by '\')
 * @next: Returns the start of the line to continue parsing
 *
 * Parse the @line into the arguments as the ccli would behave
 * when the user enters on the command line. @pargv is a pointer
 * to a string array that will be allocated and the arguments
 *  will be placed on it.
 *
 * A @delim string can be used to stop the parsing, and the last
 * value in @argv will not contain it.
 *
 * @next if not NULL will return the location in @line to run
 * the parsing again. It does not point to the copied string used
 * by @argv. If this is called multiple times, each time @argv
 * must be freed with ccli_free_argv()
 *
 * Returns the number of arguments that were parsed out of
 *    line or -1 on error. For all return values greater than zero
 *    @pargv should be freed with ccli_argv_free(), but it does
 *    not need to be called on the return of zero or less.
 */
int ccli_line_parse_multi(const char *line, char ***pargv,
			  const char *delim, const char **next)
{
	bool escape = false;
	char **argv = NULL;
	char *arg;
	char **v;
	const char *p = line;
	const char *word;
	char *w;
	char *u;
	char q = 0;
	int argc = 0;
	int dlen = 0;
	int len;

	if (!pargv) {
		errno = EINVAL;
		return -1;
	}

	if (delim)
		dlen = strlen(delim);

	/* In case ccli_argv_free() gets called on a return of zero */
	*pargv = NULL;

	while (*p) {
		bool last = false;

		/* escaped space is an argument! */
		while (!escape && ISSPACE(*p))
			p++;

		if (!*p)
			break;

		if (!escape && *p == '\\') {
			escape = true;
			p++;
			if (*p)
				p++;
			continue;
		}
		escape = false;

		word = p;

		if (match_delim(word, delim, dlen))
			break;

		for ( ; !last && *p && (q || !match_delim(p, delim, dlen)); p++) {

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
				if (!*p || (!q && match_delim(p, delim, dlen)))
					p--;
				break;

			default:
				if (q)
					break;
				if (ISSPACE(*p))
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

		/* Add two, one for a NULL value at the end */
		v = realloc(argv, sizeof(*v) * (argc + 2));
		if (!v) {
			free(arg);
			goto fail;
		}
		argv = v;

		strncpy(arg, word, len);
		arg[len] = '\0';

		/* Now remove quotes and backslashes */
		u = w = arg;
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
				*w++ = *u;
				break;
			}
		}
		*w = '\0';
		argv[argc++] = arg;
		argv[argc] = NULL;
	}

	if (next && match_delim(p, delim, dlen)) {
		p += strlen(delim);
		while (ISSPACE(*p))
			p++;
		*next = p;
	}

	*pargv = argv;

	return argc;

 fail:
	free_argv(argc, argv);
	return -1;
}

/**
 * ccli_line_parse - parse a string into its arguments
 * @line: The string to parse
 * @pargv: A pointer to place the array of strings
 *
 * Parse the @line into the arguments as the ccli would behave
 * when the user enters on the command line. @pargv is a pointer
 * to a string array that will be allocated and the arguments
 *  will be placed on it.
 *
 * Returns the number of arguments that were parsed out of
 *    line or -1 on error. For all return values greater than zero
 *    @pargv should be freed with ccli_argv_free(), but it does
 *    not need to be called on the return of zero or less.
 */
int ccli_line_parse(const char *line, char ***pargv)
{
	return ccli_line_parse_multi(line, pargv, NULL, NULL);
}

/**
 * ccli_free_argv - free a list of strings
 * @argv: The list of strings returned by ccli_line_parse()
 *
 * Free the list of strings that was allocatde by ccli_line_parse.
 */
void ccli_argv_free(char **argv)
{
	int i;

	if (!argv)
		return;

	for (i = 0; argv[i]; i++)
		free(argv[i]);
	free(argv);
}

__hidden int line_parse(const char *line, char ***pargv,
			const char *delim, const char **next)
{
	return ccli_line_parse_multi(line, pargv, delim, next);
}

__hidden void line_replace(struct line_buf *line, char *str)
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

__hidden void line_refresh(struct ccli *ccli, struct line_buf *line, int pad)
{
	char padding[pad + 3];
	int len;

	/* Just append two spaces */
	pad += 2;

	memset(padding, ' ', pad);
	padding[pad] = '\0';

	echo(ccli, '\r');

	if (line->start)
		echo_str(ccli, "> ");
	else
		echo_prompt(ccli);
	echo_str(ccli, line->line + line->start);
	echo_str(ccli, padding);
	while (pad--)
		echo(ccli, '\b');

	for (len = line->len; len > line->pos; len--)
		echo(ccli, '\b');
}
