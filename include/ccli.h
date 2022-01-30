// SPDX-License-Identifier: LGPL-2.1
#ifndef __CCLI__H
#define __CCLI__H
/*
 * CLI interface for C.
 *
 * Copyright (C) 2022 Steven Rostedt <rostedt@goodmis.org>
 */

#include <stdbool.h>

#define CCLI_NOSPACE	1

struct ccli;

typedef int (*ccli_command_callback)(struct ccli *ccli, const char *command,
				     const char *line, void *data,
				     int argc, char **argv);

typedef int (*ccli_completion)(struct ccli *ccli, const char *command,
			       const char *line, int word, char *match,
			       char ***list, void *data);

typedef int (*ccli_interrupt)(struct ccli *ccli, const char *line,
			      int pos, void *data);

struct ccli *ccli_alloc(const char *prompt, int in, int out);
void ccli_free(struct ccli *ccli);

int ccli_in(struct ccli *ccli);
int ccli_out(struct ccli *ccli);

__attribute__((__format__(printf, 2, 3)))
int ccli_printf(struct ccli *ccli, const char *fmt, ...);
int ccli_vprintf(struct ccli *ccli, const char *fmt, va_list ap);

__attribute__((__format__(printf, 3, 4)))
int ccli_page(struct ccli *ccli, int line, const char *fmt, ...);

int ccli_loop(struct ccli *ccli);
int ccli_register_command(struct ccli *ccli, const char *command_name,
			  ccli_command_callback callback, void *data);

int ccli_register_completion(struct ccli *ccli, const char *command_name,
			     ccli_completion completion);

int ccli_register_default(struct ccli *ccli, ccli_command_callback callback,
			  void *data);

int ccli_register_unknown(struct ccli *ccli, ccli_command_callback callback,
			  void *data);

int ccli_register_interrupt(struct ccli *ccli, ccli_interrupt callback,
			    void *data);

int ccli_unregister_command(struct ccli *ccli, const char *command);

int ccli_line_parse(const char *line, char ***argv);
void ccli_argv_free(char **argv);

void ccli_line_clear(struct ccli *ccli);
int ccli_line_inject(struct ccli *ccli, const char *str, int pos);

void ccli_line_refresh(struct ccli *ccli);

const char *ccli_history(struct ccli *ccli, int past);
int ccli_history_save_fd(struct ccli *ccli, const char *name, int fd);

int ccli_getchar(struct ccli *ccli);

int ccli_execute(struct ccli *ccli, const char *line, bool hist);

int ccli_history_load(struct ccli *ccli, const char *tag);
int ccli_history_save(struct ccli *ccli, const char *tag);
int ccli_history_load_file(struct ccli *ccli, const char *tag, const char *file);
int ccli_history_save_file(struct ccli *ccli, const char *tag, const char *file);
int ccli_history_load_fd(struct ccli *ccli, const char *tag, int fd);
int ccli_history_save_fd(struct ccli *ccli, const char *tag, int fd);

#endif
