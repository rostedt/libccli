/* SPDX-License-Identifier: LGPL-2.1 */
#ifndef __CCLI_LOCAL_H
#define __CCLI_LOCAL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

#include <ccli.h>

#define __hidden __attribute__((visibility ("hidden")))

#define ISSPACE(c) isspace((unsigned char)(c))

struct line_buf {
	char *line;
	int size;
	int len;
	int pos;
};

extern int line_init(struct line_buf *line);
extern int line_init_str(struct line_buf *line, const char *str);
extern void line_reset(struct line_buf *line);
extern void line_cleanup(struct line_buf *line);
extern int line_insert(struct line_buf *line, char ch);
extern void line_right(struct line_buf *line);
extern void line_left(struct line_buf *line);
extern void line_backspace(struct line_buf *line);
extern void line_del(struct line_buf *line);
extern int line_copy(struct line_buf *dst, struct line_buf *src, int len);
extern int line_parse(struct line_buf *line, char ***pargv);
extern void line_replace(struct line_buf *line, char *str);

extern void free_argv(int argc, char **argv);

#endif
