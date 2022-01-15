// SPDX-License-Identifier: LGPL-2.1
#ifndef __CCLI__H
#define __CCLI__H
/*
 * CLI interface for C.
 *
 * Copyright: Steven Rostedt <rostedt@goodmis.org>
 */

struct ccli;

typedef int (*ccli_command_callback)(struct ccli *ccli, const char *command,
				     const char *line, void *data,
				     int argc, char **argv);

struct ccli *ccli_alloc(const char *prompt, int in, int out);
void ccli_free(struct ccli *ccli);

int ccli_loop(struct ccli *ccli);
int ccli_register_command(struct ccli *ccli, const char *command_name,
			  ccli_command_callback callback, void *data);

#endif
