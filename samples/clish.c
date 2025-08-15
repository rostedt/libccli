// SPDX-License-Identifier: MIT
/*
 * Read a ccli history file
 * Copyright (C) 2023 Google Inc, Steven Rostedt <rostedt@goodmis.org>
 */
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>

#include "ccli.h"

#define PROMPT "clish> "

#define CLISH_TAG "clish"

static const char *PATH;
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

static int execute_command(struct ccli *ccli, char **argv)
{
	int status;
	int pid;

	pid = fork();
	if (pid < 0)
		return -1;
	if (pid > 0) {
		int ret;

 again:
		ret = waitpid(pid, &status, 0);
		if (ret < 0 && errno == EINTR) {
			ccli_printf(ccli, "Received ^C, killing %d\n", pid);
			kill(pid, SIGINT);
			goto again;
		}
		/* Put the console back */
		ccli_console_acquire(ccli);
		return WEXITSTATUS(status);
	}

	ccli_console_release(ccli);
	execvpe(argv[0], argv, environ);
	exit(-1);
}

/* Do not save our history on exit */
static int do_lls(struct ccli *ccli, const char *command,
		 const char *line, void *data,
		 int argc, char **argv)
{
	ccli_printf(ccli, "TODO: implement ls\n");
	return 0;
}

static int lls_completion(struct ccli *ccli, const char *command,
			      const char *line, int word, char *match,
			      char ***list, void *data)
{
	const char *path = "./";
	int cnt = 0;

	return ccli_file_completion(ccli, list, &cnt, match, 0, NULL, path);
}

/* A number command will show past history */
static int do_unknown(struct ccli *ccli, const char *command,
		      const char *line, void *data,
		      int argc, char **argv)
{
	int ret;

	if (argc < 1) // should never happen
		return 0;

	ret = execute_command(ccli, argv);
	if (ret)
		ccli_printf(ccli, "%s: command not found\n", argv[0]);

	return 0;
}

static int command_completion(struct ccli *ccli, const char *command,
			      const char *line, int word, char *match,
			      char ***list, void *data)
{
	const char *path = "./";
	int cnt = 0;

	if (!word)
		path = PATH;

	return ccli_file_completion(ccli, list, &cnt, match, 0, NULL, path);
}

static void sig_handle(int sig)
{
	/* do nothing */
}

CCLI_DEFINE_COMPLETION(command_lls, "lls", do_lls, lls_completion);

int main (int argc, char **argv)
{
	struct sigaction act = { .sa_handler = sig_handle };
	struct ccli *ccli;

	PATH = getenv("PATH");
	argv0 = argv[0];

	ccli = ccli_alloc(PROMPT, STDIN_FILENO, STDOUT_FILENO);
	if (!ccli)
		pdie("Creating command line interface");

	/* Load our own history */
	ccli_history_load(ccli, CLISH_TAG);
	ccli_alias_load(ccli, CLISH_TAG);

	ccli_register_command_delimiter(ccli, ";");

	ccli_register_command_table(ccli, &command_lls, NULL);

	ccli_register_unknown(ccli, do_unknown, NULL);
	ccli_register_default_completion(ccli, command_completion, NULL);

	/* Handle SIGINT */
	sigaction(SIGINT, &act, NULL);

	ccli_loop(ccli);

	ccli_history_save(ccli, CLISH_TAG);
	ccli_alias_save(ccli, CLISH_TAG);

	ccli_free(ccli);

	exit(0);

	return 0;
}
