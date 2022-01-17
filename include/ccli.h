// SPDX-License-Identifier: LGPL-2.1
#ifndef __CCLI__H
#define __CCLI__H
/*
 * CLI interface for C.
 *
 * Copyright (C) 2022 Steven Rostedt <rostedt@goodmis.org>
 */

struct ccli;

typedef int (*ccli_command_callback)(struct ccli *ccli, const char *command,
				     const char *line, void *data,
				     int argc, char **argv);

typedef int (*ccli_completion)(struct ccli *ccli, const char *command,
			       const char *line, int word, const char *match,
			       char ***list, void *data);

struct ccli *ccli_alloc(const char *prompt, int in, int out);
void ccli_free(struct ccli *ccli);

__attribute__((__format__(printf, 2, 3)))
int ccli_printf(struct ccli *ccli, const char *fmt, ...);

int ccli_loop(struct ccli *ccli);
int ccli_register_command(struct ccli *ccli, const char *command_name,
			  ccli_command_callback callback, void *data);

int ccli_register_completion(struct ccli *ccli, const char *command_name,
			     ccli_completion completion);

int ccli_register_default(struct ccli *ccli, ccli_command_callback callback,
			  void *data);

int ccli_register_unknown(struct ccli *ccli, ccli_command_callback callback,
			  void *data);


int ccli_parse_line(const char *line, char ***argv);
void ccli_argv_free(char **argv);

#endif
