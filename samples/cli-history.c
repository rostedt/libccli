// SPDX-License-Identifier: LGPL-2.1
/*
 * Read a ccli history file
 * Copyright (C) 2022 Google Inc, Steven Rostedt <rostedt@goodmis.org>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>

#include "ccli.h"

#define H_PROMPT "history> "

static char *argv0;

static char *get_this_name(void)
{
	static char *this_name;
	char *arg;
	char *p;

	if (this_name)
		return this_name;

	arg = argv0;
	p = arg+strlen(arg);

	while (p >= arg && *p != '/')
		p--;
	p++;

	this_name = p;
	return p;
}

static void usage(void)
{
	char *p = get_this_name();

	printf("usage: %s file\n"
	       "\n",p);
	exit(-1);
}

static void __vdie(const char *fmt, va_list ap, int err)
{
	int ret = errno;
	char *p = get_this_name();

	if (err && errno)
		perror(p);
	else
		ret = -1;

	fprintf(stderr, "  ");
	vfprintf(stderr, fmt, ap);

	fprintf(stderr, "\n");
	exit(ret);
}

static void pdie(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	__vdie(fmt, ap, 1);
	va_end(ap);
}

struct history {
	char		*file;
	char		*tag;
	bool		write_history;
};

/* Save the history of the tag and file given */
static int save_history(struct ccli *ccli, const char *command,
			const char *line, void *data,
			int argc, char **argv)
{
	struct history *h = data;

	if (h->file)
		ccli_history_save_file(ccli, h->tag, h->file);
	else
		ccli_history_save(ccli, h->tag);

	return 0;
}

/* Do not save our history on exit */
static int no_history(struct ccli *ccli, const char *command,
			const char *line, void *data,
			int argc, char **argv)
{
	struct history *h = data;

	h->write_history = false;

	return 0;
}

/* Save our history on exit */
static int yes_history(struct ccli *ccli, const char *command,
		       const char *line, void *data,
		       int argc, char **argv)
{
	struct history *h = data;

	h->write_history = true;

	return 0;
}

/* A number command will show past history */
static int do_unknown(struct ccli *ccli, const char *command,
		      const char *line, void *data,
		      int argc, char **argv)
{
	const char *hist;
	char *arg;
	int i;

	if (argc < 1) // should never happen
		return 0;

	arg = argv[0];

	if (!isdigit(arg[0])) {
		ccli_printf(ccli, "Type a number for past history\n");
		return 0;
	}

	i = atoi(argv[0]);

	hist = ccli_history(ccli, i);
	if (hist)
		ccli_printf(ccli, "History %d ago: %s\n", i, hist);
	else
		ccli_printf(ccli, "No history at %d\n", i);

	return 0;
}

int main (int argc, char **argv)
{
	struct ccli *ccli;
	struct history h;
	char *file = NULL;
	char *tag;

	argv0 = argv[0];

	if (argc < 2)
		usage();
	tag = argv[1];

	if (argc > 2)
		file = argv[2];

	h.tag = tag;
	h.file = file;
	h.write_history = true;

	ccli = ccli_alloc(H_PROMPT, STDIN_FILENO, STDOUT_FILENO);
	if (!ccli)
		pdie("Creating command line interface");

	/* Load our own history */
	ccli_history_load(ccli, "cli-history");

	if (file)
		ccli_history_load_file(ccli, tag, file);
	else
		ccli_history_load(ccli, tag);

	ccli_register_command(ccli, "save", save_history, &h);
	ccli_register_command(ccli, "no_history", no_history, &h);
	ccli_register_command(ccli, "yes_history", yes_history, &h);

	ccli_register_unknown(ccli, do_unknown, NULL);

	ccli_loop(ccli);
	if (h.write_history)
		ccli_history_save(ccli, "cli-history");

	ccli_free(ccli);

	exit(0);

	return 0;
}
