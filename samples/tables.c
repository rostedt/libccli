// SPDX-License-Identifier: LGPL-2.1
/*
 * Read a ccli history file
 * Copyright (C) 2023 Google Inc, Steven Rostedt <rostedt@goodmis.org>
 */
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>

#include "ccli.h"

#define PROMPT "cmd> "

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

struct io_item {
	struct io_item		*next;
	char			*name;
};

static struct io_item *files;
static struct io_item *dirs;

static struct io_item *find_item(const char *name, struct io_item *items)
{
	struct io_item *item;

	for (item = items; item; item = item->next) {
		if (strcmp(item->name, name) == 0)
			return item;
	}

	return NULL;
}
static struct io_item *find_file(const char *file)
{
	return find_item(file, files);
}

static struct io_item *find_dir(const char *dir)
{
	return find_item(dir, dirs);
}

static void remove_io_item(struct ccli *ccli, const char *name, struct io_item **list)
{
	struct io_item **last_item, *item;
	bool found = false;

	for (last_item = list; *last_item; last_item = &(*last_item)->next) {
		item = *last_item;
		if (strcmp(item->name, name) == 0) {
			*last_item = item->next;
			free(item->name);
			free(item);
			found = true;
			ccli_printf(ccli, "Removed %s\n", name);
			break;
		}
	}
	if (!found)
		ccli_printf(ccli, "%s not loaded\n", name);
};

static int dump_command(struct ccli *ccli, const char *command,
		const char *line, void *data,
		int argc, char **argv)
{
	struct io_item *item;
	char *fline = NULL;
	size_t n = 0;
	ssize_t r;
	FILE *fp;
	int cnt = 1;
	int i;

	if (argc < 2) {
		ccli_printf(ccli, "usage: dump <file> [ <file2> .. ]\n");
		return 0;
	}

	for (i = 1; i < argc; i++) {
		item = find_file(argv[1]);
		if (!item) {
			ccli_printf(ccli, "%s not loaded\n", argv[1]);
			return 0;
		}
	}

	for (i = 1; i < argc; i++) {
		fp = fopen(argv[1], "r");
		if (fp == NULL) {
			ccli_printf(ccli, "%s: %s\n", argv[1], strerror(errno));
			return 0;
		}

		while (cnt > 0 && (r = getline(&fline, &n, fp)) >= 0)
			cnt = ccli_page(ccli, cnt, "%s", fline);

		free(fline);
		fclose(fp);
	}
	return 0;
}

static int list_command(struct ccli *ccli, const char *command,
			const char *line, void *data,
			int argc, char **argv)
{
	struct io_item *item;
	int i;

	if (argc < 2) {
		ccli_printf(ccli, "usage: list <dir> [ <dir2> ..]\n");
		return 0;
	}

	for (i = 1; i < argc; i++) {
		item = find_dir(argv[1]);
		if (!item) {
			ccli_printf(ccli, "%s not loaded\n", argv[1]);
			return 0;
		}
	}

	for (i = 1; i < argc; i++) {
		struct dirent *dirent;
		DIR *dir;
		char **list = NULL;
		int max_len = 0;
		int cnt = 0;

		dir = opendir(argv[i]);
		if (!dir) {
			ccli_printf(ccli, "%s: %s\n", argv[i], strerror(errno));
			break;
		}
		while ((dirent = readdir(dir))) {
			int len;

			if (strcmp(dirent->d_name, ".") == 0 ||
			    strcmp(dirent->d_name, "..") == 0)
				continue;
			len = strlen(dirent->d_name);
			if (len > max_len)
				max_len = len;
			ccli_list_add(ccli, &list, &cnt, dirent->d_name);
		}
		closedir(dir);

		ccli_print_list(ccli, list, cnt, max_len);
		ccli_list_free(ccli, &list, cnt);
	}

	return 0;
}

static int add_cmd(struct ccli *ccli, const char *type_name, int argc, char **argv,
		   int mode, struct io_item **list)
{
	char tname[strlen(type_name) + 1];
	struct io_item *item;
	struct stat st;
	int i;

	strcpy(tname, type_name);
	tname[0] = toupper(tname[0]);

	for (i = 1; i < argc; i++) {
		if (stat(argv[i], &st) < 0) {
			ccli_printf(ccli, "%s %s not found\n", tname, argv[i]);
			continue;
		}
		if ((st.st_mode & S_IFMT) != mode) {
			ccli_printf(ccli, "%s is not a %s\n", argv[i], type_name);
			continue;
		}

		item = find_item(argv[i], *list);
		if (item) {
			ccli_printf(ccli, "%s is already loaded=n", argv[i]);
			continue;
		}

		item = calloc(1, sizeof(*item));
		if (!item) {
			ccli_printf(ccli, "Failed allocation\n");
			break;
		}
		item->name = strdup(argv[i]);
		if (!item->name) {
			free(item);
			ccli_printf(ccli, "Failed allocation\n");
			break;
		}
		ccli_printf(ccli, "Added %s\n", item->name);
		item->next = *list;
		*list = item;
	}

	return 0;
}

static int open_file_command(struct ccli *ccli, const char *command,
			     const char *line, void *data,
			     int argc, char **argv)
{
	if (argc < 2) {
		ccli_printf(ccli, "usage: open file <file1> [<file2> ..]\n");
		return 0;
	}

	return add_cmd(ccli, "file", argc, argv, S_IFREG, &files);
}

static int open_dir_command(struct ccli *ccli, const char *command,
			    const char *line, void *data,
			    int argc, char **argv)
{
	if (argc < 2) {
		ccli_printf(ccli, "usage: open dir <dir> [<dir2> ..]\n");
		return 0;
	}

	return add_cmd(ccli, "directory", argc, argv, S_IFDIR, &dirs);
}

static int close_file_command(struct ccli *ccli, const char *command,
			      const char *line, void *data,
			      int argc, char **argv)
{
	int i;

	if (argc < 2) {
		ccli_printf(ccli, "close file <file> [ <file2> ..]\n");
		return 0;
	}

	for (i = 1; i < argc; i++) {
		remove_io_item(ccli, argv[i], &files);
	}
	return 0;
}

static int close_dir_command(struct ccli *ccli, const char *command,
			     const char *line, void *data,
			     int argc, char **argv)
{
	int i;

	if (argc < 2) {
		ccli_printf(ccli, "close dir <dir> [ <dir> ..]\n");
		return 0;
	}

	for (i = 1; i < argc; i++) {
		remove_io_item(ccli, argv[i], &dirs);
	}
	return 0;
}

static CCLI_DEFINE_COMMAND(command_dump, "dump", dump_command);
static CCLI_DEFINE_COMMAND(command_list, "list", list_command);
static CCLI_DEFINE_COMMAND(command_open_file, "file", open_file_command);
static CCLI_DEFINE_COMMAND(command_open_dir, "dir", open_dir_command);
static CCLI_DEFINE_COMMAND(command_close_file, "file", close_file_command);
static CCLI_DEFINE_COMMAND(command_close_dir, "dir", close_dir_command);
static CCLI_DEFINE_COMMAND(command_open, "open", NULL,
		    &command_open_file, &command_open_dir);
static CCLI_DEFINE_COMMAND(command_close, "close", NULL,
		    &command_close_file, &command_close_dir);
static CCLI_DEFINE_COMMAND(command_main, NULL, NULL,
		    &command_open, &command_close, &command_dump, &command_list);

static int list_items(struct ccli *ccli, char ***list, char **prev, int nr_prev, struct io_item *items)
{
	struct io_item *item;
	int cnt = 0;
	int ret = 0;
	int i;

	for (item = items; ret >= 0  && item; item = item->next) {
		bool found = false;

		for (i = 0; i < nr_prev; i++) {
			if (strcmp(item->name, prev[i]) == 0) {
				found = true;
				break;
			}
		}
		if (found)
			continue;
		ret = ccli_list_add(ccli, list, &cnt, item->name);
	}
	return ret;
}

static int list_files(struct ccli *ccli, char ***list, char **prev, int nr_prev)
{
	return list_items(ccli, list, prev, nr_prev, files);
}

static int list_dirs(struct ccli *ccli, char ***list, char **prev, int nr_prev)
{
	return list_items(ccli, list, prev, nr_prev, dirs);
}

static int close_completion_file(struct ccli *ccli, const char *command,
				 const char *line, int word, char *match,
				 char ***list, void *data)
{
	char **argv;
	int argc;
	int ret;

	argc = ccli_line_parse(line, &argv);
	ret = list_files(ccli, list, argv + 2, argc - 2);
	ccli_argv_free(argv);
	return ret;
}

static int close_completion_dir(struct ccli *ccli, const char *command,
				const char *line, int word, char *match,
				char ***list, void *data)
{
	char **argv;
	int argc;
	int ret;

	argc = ccli_line_parse(line, &argv);
	ret = list_dirs(ccli, list, argv + 2, argc - 2);
	ccli_argv_free(argv);
	return ret;
	return 0;
}

static int close_completion(struct ccli *ccli, const char *command,
			    const char *line, int word, char *match,
			    char ***list, void *data)
{
	return 0;
}

static CCLI_DEFINE_COMPLETION(completion_close_file, "file", close_completion_file);
static CCLI_DEFINE_COMPLETION(completion_close_dir, "dir", close_completion_dir);
static CCLI_DEFINE_COMPLETION(completion_close, "close", close_completion,
	&completion_close_file, &completion_close_dir);

const int open_completion_file(struct ccli *ccli, const char *command,
			 const char *line, int word, char *match,
			 char ***list, void *data)
{
	int cnt = 0;

	return ccli_file_completion(ccli, list, &cnt, match, S_IFREG, NULL, ".");
}

const int open_completion_dir(struct ccli *ccli, const char *command,
			const char *line, int word, char *match,
			char ***list, void *data)
{
	int cnt = 0;

	return ccli_file_completion(ccli, list, &cnt, match, S_IFDIR, NULL, ".");
}

static CCLI_DEFINE_COMPLETION(completion_open_file, "file", open_completion_file);
static CCLI_DEFINE_COMPLETION(completion_open_dir, "dir", open_completion_dir);
static CCLI_DEFINE_COMPLETION(completion_open, "open", NULL,
	&completion_open_file, &completion_open_dir);

const int dump_completion(struct ccli *ccli, const char *command,
			  const char *line, int word, char *match,
			  char ***list, void *data)
{
	char **argv;
	int argc;
	int ret;

	argc = ccli_line_parse(line, &argv);
	ret = list_files(ccli, list, argv + 1, argc - 1);
	ccli_argv_free(argv);
	return ret;
}

static CCLI_DEFINE_COMPLETION(completion_dump, "dump", dump_completion);

const int list_completion(struct ccli *ccli, const char *command,
			  const char *line, int word, char *match,
			  char ***list, void *data)
{
	char **argv;
	int argc;
	int ret;

	argc = ccli_line_parse(line, &argv);
	ret = list_dirs(ccli, list, argv + 1, argc - 1);
	ccli_argv_free(argv);
	return ret;
}

static CCLI_DEFINE_COMPLETION(completion_list, "list", list_completion);
static CCLI_DEFINE_COMPLETION(completion_main, NULL, NULL,
		       &completion_open, &completion_close,
		       &completion_list, &completion_dump);

int main (int argc, char **argv)
{
	struct ccli *ccli;

	argv0 = argv[0];

	ccli = ccli_alloc(PROMPT, STDIN_FILENO, STDOUT_FILENO);
	if (!ccli)
		pdie("Creating command line interface");

	ccli_register_command_table(ccli, &command_main, NULL);
	ccli_register_completion_table(ccli, &completion_main, NULL);

	ccli_loop(ccli);

	ccli_free(ccli);

	exit(0);

	return 0;
}
