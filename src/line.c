// SPDX-License-Identifier: LGPL-2.1
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

__hidden int line_parse(struct line_buf *line, char ***pargv)
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
