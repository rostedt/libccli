// SPDX-License-Identifier: LGPL-2.1
/*
 * Copyright (C) 2022 Steven Rostedt <rostedt@goodmis.org>
 */
#include "ccli-local.h"

__hidden void free_argv(int argc, char **argv)
{
	printf("freeing %d elements\n", argc);
	for (argc--; argc >= 0; argc--) {
		printf("freeing: %s\n", argv[argc]);
		free(argv[argc]);
	}
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

__hidden void line_reset(struct line_buf *line)
{
	memset(line->line, 0, line->size);
	line->len = 0;
	line->pos = 0;
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

__hidden void line_right(struct line_buf *line)
{
	if (line->pos < line->len)
		line->pos++;
}

__hidden void line_left(struct line_buf *line)
{
	if (line->pos)
		line->pos--;
}

__hidden void line_backspace(struct line_buf *line)
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
int ccli_line_parse(const char *line, struct command_args **pcmd_args)
{
	char **argv = NULL;
	char *arg;
	char **v;
	const char *p = line;
	const char *word;
	char *w;
	char *u;
	char q = 0;
	int argc = 0;
	int len;
	int cmd_idx = 0;

	if (!pcmd_args) {
		errno = -EINVAL;
		return -1;
	}

	/* In case ccli_argv_free() gets called on a return of zero */
	*pcmd_args = NULL;

	while (*p) {
		bool last = false;

		while (ISSPACE(*p))
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

		if (strcmp(arg, "&&") == 0) {
			if (cmd_idx == 0) {
				*pcmd_args = (struct command_args *) malloc(sizeof(struct command_args));
			} else {
				*pcmd_args = (struct command_args *) realloc(*pcmd_args, sizeof(struct command_args) * (cmd_idx + 1));
			}
			(*pcmd_args + cmd_idx)->argv = argv;
			(*pcmd_args + cmd_idx)->argc = argc;
			argc = 0;
			argv = malloc(sizeof(*v)); // TODO what to do here?
			++cmd_idx;
		} else {
			argv[argc++] = arg;
			argv[argc] = NULL;
		}
	}

	if (cmd_idx == 0) {
		*pcmd_args = (struct command_args *) malloc(sizeof(struct command_args));
	} else {
		*pcmd_args = (struct command_args *) realloc(*pcmd_args, sizeof(struct command_args) * (cmd_idx + 1));
	}
	(*pcmd_args + cmd_idx)->argv = argv;
	(*pcmd_args + cmd_idx)->argc = argc;
 
	return cmd_idx + 1;

 fail:
	free_argv(argc, argv);
	return -1;
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

__hidden int line_parse(struct line_buf *line, struct command_args **pcmd_args)
{
	return ccli_line_parse(line->line, pcmd_args);
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
