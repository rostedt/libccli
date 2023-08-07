// SPDX-License-Identifier: LGPL-2.1
/*
 * Copyright (C) 2022-2023 Steven Rostedt <rostedt@goodmis.org>
 * Copyright (C) 2023 Google, Steven Rostedt <rostedt@goodmis.org>
 */
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>

#include "ccli-local.h"

#define CCLI_ALIAS_LINE_START \
	"####---ccli---####"

#define CCLI_ALIAS_LINE_END \
	"%%%%---ccli---%%%%"

#define CCLI_NAME "ccli-alias"

__hidden struct alias *find_alias(struct ccli *ccli, const char *alias)
{
	int i;

	for (i = 0; i < ccli->nr_aliases; i++) {
		if (strcmp(alias, ccli->aliases[i].alias) == 0)
			return &ccli->aliases[i];
	}

	return NULL;
}

static int remove_alias(struct ccli *ccli, const char *alias_name)
{
	struct alias *alias;
	int cnt;

	if (!ccli || !alias_name) {
		errno = EINVAL;
		return -1;
	}

	alias = find_alias(ccli, alias_name);
	if (!alias)
		return -1;

	free(alias->alias);
	free(alias->command);

	cnt = (ccli->nr_aliases - (alias - ccli->aliases)) - 1;
	if (cnt)
		memmove(alias, alias + 1, cnt * sizeof(*alias));
	ccli->nr_aliases--;
	alias = &ccli->aliases[ccli->nr_aliases];
	memset(alias, 0, sizeof(*alias));

	return 0;
}

/**
 * ccli_register_alias - Register an alias for the CLI interface.
 * @ccli: The CLI descriptor to register a command for.
 * @alias_name: the text that will execute the alias command
 * @command: The string that the @alias will represent
 *
 * Register an alias string that will act as an alias. When the use
 * types this in as their first line, it will be converted into the
 * corresponding @command. An @alias can reference a command with the
 * same name as the @alias, but it can not reference other aliases.
 *
 * Returns 0 on success and -1 on error.
 */
int ccli_register_alias(struct ccli *ccli, const char *alias_name,
			const char *command)
{
	struct alias *alias;
	char *new_command;
	char *new_alias;

	if (!ccli || !alias_name) {
		errno = EINVAL;
		return -1;
	}

	/* Check if we are to remove an alias */
	if (!command || !strlen(command))
		return remove_alias(ccli, alias_name);

	new_command = strdup(command);
	if (!new_command)
		return -1;

	alias = find_alias(ccli, alias_name);
	if (alias) {
		free(alias->command);
		alias->command = new_command;
		return 0;
	}

	new_alias = strdup(alias_name);
	if (!new_alias) {
		free(new_command);
		return -1;
	}

	alias = realloc(ccli->aliases,
			sizeof(*alias) * (ccli->nr_aliases + 1));
	if (!alias) {
		free(new_command);
		free(new_alias);
		return -1;
	}

	memset(&alias[ccli->nr_aliases], 0, sizeof(alias[0]));

	alias[ccli->nr_aliases].alias = new_alias;
	alias[ccli->nr_aliases].command = new_command;

	ccli->aliases = alias;
	ccli->nr_aliases++;

	return 0;
}

static int write_callback(struct ccli *ccli, int fd, int idx, int cnt, void *data)
{
	char *alias = ccli->aliases[idx].alias;
	char *command = ccli->aliases[idx].command;
	int ret;

	ret = write(fd, alias, strlen(alias));
	if (ret < strlen(alias))
		return -1;

	ret = write(fd, "=", 1);
	if (ret != 1)
		return -1;

	ret = write(fd, command, strlen(command));
	if (ret < strlen(command))
		return -1;

	ret = write(fd, "\n", 1);
	if (ret < 1)
		return -1;

	return 0;
}

/**
 * ccli_alias_save_fd - Write the aliases into the file descriptor
 * @ccli: The ccli descriptor to write the alias of
 * @tag: The tag to give this alias segment
 * @fd: The file descriptor to write the alias into
 *
 * Will write all saved aliases into the file descriptor.
 * It will first write a special line that will denote the @tag and
 * size of the alias section. The @tag is used so that multiple alias
 * sections can be loaded into the same file, and can be retrieved via the
 * @tag.
 *
 * Returns the number of alias lines written on success and -1
 *  on error.
 */
int ccli_alias_save_fd(struct ccli *ccli, const char *tag, int fd)
{
	if (!ccli || !tag || fd < 0) {
		errno = EINVAL;
		return -1;
	}

	/* Do nothing if there's no aliases */
	if (!ccli->has_alias && !ccli->nr_aliases)
		return 0;

	return cache_save_fd(ccli, CCLI_ALIAS_LINE_START,
			     CCLI_ALIAS_LINE_END, tag, fd, ccli->nr_aliases,
			     write_callback, NULL);
}

static int read_callback(struct ccli *ccli, char *line, int ret, int idx,
			 int cnt, void *data)
{
	char *save_ptr;
	char *alias;
	char *command;

	/* Ignore empty lines */
	if (!ret)
		return 0;

	alias = strtok_r(line, "=", &save_ptr);
	command = strtok_r(NULL, "", &save_ptr);
	if (!alias || !command)
		return -1;

	return ccli_register_alias(ccli, alias, command);
}

/**
 * ccli_alias_load_fd - Read the aliases from the file descriptor
 * @ccli: The ccli descriptor to read the aliases into
 * @tag: The tag to use to find in the file
 * @fd: The file descriptor to read the aliases from
 *
 * Will read the file descriptor looking for the start of aliases
 * that matches the @tag. Then it will load the found aliases into
 * the @ccli.
 *
 * Returns the number of alias lines read on success and -1
 *  on error.
 */
int ccli_alias_load_fd(struct ccli *ccli, const char *tag, int fd)
{
	int ret;

	if (!ccli || !tag || fd < 0) {
		errno = EINVAL;
		return -1;
	}

	ret = cache_load_fd(ccli, CCLI_ALIAS_LINE_START,
			    CCLI_ALIAS_LINE_END, tag, fd, read_callback, NULL);

	/* May need to clear aliases */
	if (ret > 0)
		ccli->has_alias = true;

	return ret;
}

/**
 * ccli_alias_save_file - Write the saved aliases into a file
 * @ccli: The ccli descriptor to write the aliases of
 * @tag: The tag to give this alias segment
 * @file: The file path to write to
 *
 * It will first search for the @tag, and if it is there, it will
 * remove the contents of that @tag and replace it with the
 * new content.
 * Will write the saved aliases into the @file path.
 * It will first write a special line that will denote the @tag and
 * size of the aliases. The @tag is used so that multiple aliases
 * can be loaded into the same file, and can be retrieved via the
 * @tag.
 *
 * Returns the number of alias lines written on success and -1
 *  on error.
 */
int ccli_alias_save_file(struct ccli *ccli, const char *tag, const char *file)
{
	/* Do nothing if there's no aliases */
	if (!ccli->has_alias && !ccli->nr_aliases)
		return 0;

	return cache_save_file(ccli, CCLI_ALIAS_LINE_START,
			       CCLI_ALIAS_LINE_END, tag, file, ccli->nr_aliases,
			       write_callback, NULL);
}

/**
 * ccli_alias_load_file - Read the aliases from the given file path
 * @ccli: The ccli descriptor to read the aliases into
 * @tag: The tag to use to find in the file
 * @file: The file path to read the aliases from
 *
 * Will read the @file looking for the start of the aliases section
 * that matches the @tag. Then it will load the found aliases into
 * the @ccli.
 *
 * Returns the number of alias lines read on success and -1
 *  on error.
 */
int ccli_alias_load_file(struct ccli *ccli, const char *tag, const char *file)
{
	int ret;
	int fd;

	fd = open(file, O_RDONLY);
	if (fd < 0)
		return -1;

	ret = ccli_alias_load_fd(ccli, tag, fd);

	close(fd);
	return ret;
}

/**
 * ccli_alias_save - Write the aliases into the default ccli file
 * @ccli: The ccli descriptor to write the aliases of
 * @tag: The tag to give this alias segment
 *
 * Will write the saved aliases into the the default
 * ccli file that is located in $(XDG_CACHE_HOME)/ccli-alias or in
 * $(HOME)/.cache/ccli-alias.
 *
 * It will first write a special line that will denote the @tag and
 * size of the alias section. The @tag is used so that multiple aliases
 * can be loaded into the same file, and can be retrieved via the
 * @tag.
 *
 * Returns the number of alias lines written on success and -1
 *  on error.
 */
int ccli_alias_save(struct ccli *ccli, const char *tag)
{
	char *file;
	int ret;

	file = get_cache_file(CCLI_NAME);
	ret = ccli_alias_save_file(ccli, tag, file);
	free(file);

	return ret;
}

/**
 * ccli_alias_load - Read the aliases from the default path
 * @ccli: The ccli descriptor to read the aliases into
 * @tag: The tag to use to find in the file
 *
 * Will read the default path defined by either $XDG_CACHE_HOME/ccli-alias or
 * $HOME/.cache/ccli-alias and then look for the alias section
 * that matches the @tag. Then it will load the found aliases into
 * the @ccli.
 *
 * Returns the number of aliases lines read on success and -1
 *  on error.
 */
int ccli_alias_load(struct ccli *ccli, const char *tag)
{
	char *file;
	int ret;

	file = get_cache_file(CCLI_NAME);
	ret = ccli_alias_load_file(ccli, tag, file);
	free(file);

	return ret;
}

static int list_aliases(struct ccli *ccli)
{
	int i;

	for (i = 0; i < ccli->nr_aliases; i++) {
		ccli_printf(ccli, "alias %s='%s'\n", ccli->aliases[i].alias,
			    ccli->aliases[i].command);
	}

	return 0;
}

static void do_alias(struct ccli *ccli, char *word)
{
	struct alias *alias;
	char *eq;

	eq = strchr(word, '=');

	if (!eq) {
		alias = find_alias(ccli, word);
		if (!alias) {
			ccli_printf(ccli, "alias %s: not found\n", word);
			return;
		}

		ccli_printf(ccli, "alias %s=%s\n", word, alias->command);
		return;
	}

	*eq = '\0';

	eq++;
	ccli_register_alias(ccli, word, eq);
}

__hidden int exec_alias(struct ccli *ccli, const char *command,
			const char *line, void *data,
			int argc, char **argv)
{
	int i;

	if (argc < 2)
		return list_aliases(ccli);

	for (i = 1; i < argc; i++)
		do_alias(ccli, argv[i]);
	return 0;
}

static void do_unalias(struct ccli *ccli, char *word)
{
	struct alias *alias;

	alias = find_alias(ccli, word);
	if (!alias) {
		ccli_printf(ccli, "unalias %s: not found\n", word);
		return;
	}
	ccli_register_alias(ccli, word, NULL);
}

__hidden int exec_unalias(struct ccli *ccli, const char *command,
			const char *line, void *data,
			int argc, char **argv)
{
	int i;

	if (argc < 2) {
		/* TODO add -a option */
		ccli_printf(ccli, "unalias: usage: unalias name [name ...]\n");
		return 0;
	}

	for (i = 1; i < argc; i++)
		do_unalias(ccli, argv[i]);
	return 0;
}

__hidden int execute_alias(struct ccli *ccli, struct alias *alias,
			   const char *line, int argc, char **argv)
{
	char *new_line;
	char *p;
	int ret;
	int len;
	int i;

	if (alias->exec)
		return ccli->unknown.callback(ccli, argv[0], line,
					     ccli->unknown.data,
					     argc, argv);
	/* Detect recursion */
	alias->exec = true;
	len = strlen(alias->command) + 1;

	for (i = 1; i < argc; i++)
		len += strlen(argv[i]) + 1;

	len++;
	new_line = malloc(len);
	if (!new_line) {
		alias->exec = false;
		return -1;
	}

	strcpy(new_line, alias->command);
	p = new_line + strlen(alias->command);

	for (i = 1; i < argc; i++) {
		*p = ' ';
		p++;
		strcpy(p, argv[i]);
		p += strlen(argv[i]);
	}
	*p = '\0';

	ret = execute(ccli, new_line, false);

	free(new_line);
	alias->exec = false;

	return ret;
}
