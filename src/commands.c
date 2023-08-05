// SPDX-License-Identifier: MIT
/*
 * CLI interface for C.
 *
 * Copyright (C) 2022 Steven Rostedt <rostedt@goodmis.org>
 */
#include "ccli-local.h"

__hidden struct command *find_command(struct ccli *ccli, const char *cmd)
{
	int i;

	for (i = 0; i < ccli->nr_commands; i++) {
		if (strcmp(cmd, ccli->commands[i].cmd) == 0)
			return &ccli->commands[i];
	}

	return NULL;
}

/**
 * ccli_unregister_command - Remove a command from the CLI interface
 * @ccli: The CLI descriptor to remove the command from
 * @command_name: The command name to remove
 *
 * Removes the registered command named @command_name from @ccli.
 *
 * Returns 0 on successful removal, or -1 on error or @command_name not found.
 *   ERRNO will only be set for error and will not be touched if the
 *  @command_name was not found.
 */
int ccli_unregister_command(struct ccli *ccli, const char *command_name)
{
	struct command *commands;
	int cnt;

	if (!ccli || !command_name) {
		errno = EINVAL;
		return -1;
	}

	commands = find_command(ccli, command_name);
	if (!commands)
		return -1;

	free(commands->cmd);

	cnt = (ccli->nr_commands - (commands - ccli->commands)) - 1;
	if (cnt)
		memmove(commands, commands + 1, cnt * sizeof(*commands));
	ccli->nr_commands--;
	commands = &ccli->commands[ccli->nr_commands];
	memset(commands, 0, sizeof(*commands));

	return 0;
}

/**
 * ccli_register_command - Register a command for the CLI interface.
 * @ccli: The CLI descriptor to register a command for.
 * @command_name: the text that will execute the command.
 * @callback: The callback function to call when the command is executed.
 * @data: Data to pass to the @callback function.
 *
 * Register the command @command_name to @ccli. When the user types
 * that command, it will trigger the @callback which will be passed
 * the arguments that the user typed, along with the @data pointer.
 *
 * Returns 0 on success and -1 on error.
 */
int ccli_register_command(struct ccli *ccli, const char *command_name,
			  ccli_command_callback callback, void *data)
{
	struct command *commands;
	char *cmd;

	if (!ccli || !command_name || !callback) {
		errno = EINVAL;
		return -1;
	}

	commands = find_command(ccli, command_name);
	if (commands) {
		/* override it with this one */
		commands->callback = callback;
		commands->data = data;
		return 0;
	}

	cmd = strdup(command_name);
	if (!cmd)
		return -1;

	commands = realloc(ccli->commands,
			   sizeof(*commands) * (ccli->nr_commands + 1));
	if (!commands) {
		free(cmd);
		return -1;
	}

	memset(&commands[ccli->nr_commands], 0, sizeof(commands[0]));

	commands[ccli->nr_commands].cmd = cmd;
	commands[ccli->nr_commands].callback = callback;
	commands[ccli->nr_commands].data = data;

	ccli->commands = commands;
	ccli->nr_commands++;

	return 0;
}

/**
 * ccli_register_default - Register a callback for just "enter".
 * @ccli: The CLI descriptor to register the default for.
 * @callback: The callback function to call when just "enter" is hit.
 * @data: Data to pass to the @callback function.
 *
 * When the user hits enter with nothing on the command line, the
 * @callback function will be called just like any other command is
 * called, but the command_name parameter will be empty.
 *
 * Returns 0 on success and -1 on error.
 */
int ccli_register_default(struct ccli *ccli,
			  ccli_command_callback callback, void *data)
{
	if (!ccli || !callback) {
		errno = EINVAL;
		return -1;
	}

	ccli->enter.callback = callback;
	ccli->enter.data = data;

	return 0;
}

/**
 * ccli_register_unknown - Register a callback for unknown commands.
 * @ccli: The CLI descriptor to register the default for.
 * @callback: The callback function to call when a command is unknown
 * @data: Data to pass to the @callback function.
 *
 * When the user hits enters a command that is not registered, the
 * @callback function will be called just like any other command is
 * called.
 *
 * Returns 0 on success and -1 on error.
 */
int ccli_register_unknown(struct ccli *ccli,
			  ccli_command_callback callback, void *data)
{
	if (!ccli || !callback) {
		errno = EINVAL;
		return -1;
	}

	ccli->unknown.callback = callback;
	ccli->unknown.data = data;

	return 0;
}

/**
 * ccli_register_interrupt - Register what to do on SIGINT
 * @ccli: The CLI descriptor to register to.
 * @callback: The callback function to call when user hits Ctrl^C
 * @data: The data to pass to the callback.
 *
 * This allows the application to register a function when the user
 * hits Ctrl^C. Currently the default operation is simply to print
 * "^C\n" and exit. But this allows the application to override that
 * operation.
 *
 * Returns 0 on success and -1 on error.
 */
int ccli_register_interrupt(struct ccli *ccli, ccli_interrupt callback,
			    void *data)
{
	if (!ccli || !callback) {
		errno = EINVAL;
		return -1;
	}

	ccli->interrupt = callback;
	ccli->interrupt_data = data;
	return 0;
}

__hidden int execute(struct ccli *ccli, const char *line, bool hist)
{
	struct command *cmd;
	char **argv;
	int argc;
	int ret = 0;

	argc = line_parse(line, &argv);
	if (argc < 0) {
		echo_str(ccli, "Error parsing command\n");
		return 0;
	}

	if (!argc)
		return ccli->enter.callback(ccli, "", line,
					    ccli->enter.data,
					    0, NULL);

	cmd = find_command(ccli, argv[0]);

	if (cmd) {
		ret = cmd->callback(ccli, cmd->cmd,
				    line, cmd->data,
				    argc, argv);
	} else {
		ret = ccli->unknown.callback(ccli, argv[0], line,
					     ccli->unknown.data,
					     argc, argv);
	}

	free_argv(argc, argv);

	if (hist)
		history_add(ccli, line);

	return ret;
}

/**
 * ccli_execute - Execute a command from outside the loop
 * @ccli: The ccli descriptor
 * @line_str: The line to execute as if a user typed it.
 * @hist: Add to history or not?
 *
 * Execute a line as if the user typed it from outside the loop.
 * The application may want to initiate some commands before executing
 * the loop (like to reply commands from a previous session). This
 * gives that ability.
 *
 * If @hist is true, then the command is added to the history,
 * otherwise it is not.
 *
 * Returns whatever the command being executed would return.
 *  If a memory allocation happens, -1 is returned and ERRNO is set.
 */
int ccli_execute(struct ccli *ccli, const char *line_str, bool hist)
{
	struct line_buf *old_line = ccli->line;
	struct line_buf line;
	int ret;

	ret = line_init_str(&line, line_str);
	if (ret < 0)
		return ret;

	ccli->line = &line;
	ret = execute(ccli, line.line, hist);
	line_cleanup(&line);
	ccli->line = old_line;
	return ret;
}

static void test_table(const void *data)
{
	const struct ccli_command_table *table = data;
	const char *name = table->name;
	int len = strlen(test_name);
	int l = 0;
	int i;

	/* The root of the table does not need a name */
	if (!name)
		name = "root";

	if (len)
		strncat(test_name, "->", BUFSIZ - 1);
	strncat(test_name, name, BUFSIZ - 1);

	snprintf(test_message, BUFSIZ - 1,
		 "Command table missing name or 'subcommands' NULL terminator");

	/* Touch all the names first */
	for (i = 0; table->subcommands[i]; i++)
		l += strlen(table->subcommands[i]->name);

	snprintf(test_message, BUFSIZ - 1,
		 "Command table missing 'options' NULL terminator");

	if (table->options) {
		int clen;

		/* Test the options too */

		clen = strlen(test_name);
		if (clen)
			strncat(test_name, "->[options]", BUFSIZ - 1);

		for (i = 0; table->options->options[i]; i++)
			l += strlen(table->options->options[i]->name);

		test_name[clen] = '\0';
	}

	/* Now recurse */
	for (i = 0; table->subcommands[i]; i++)
		test_table(table->subcommands[i]);

	test_name[len] = '\0';
}

static const struct ccli_command_table *
find_table_command(const struct ccli_command_table *command,
		   int argc, char **argv, int *level)
{
	const struct ccli_command_table *subcommand;
	int i;

	if (argc > 1) {
		for (i = 0; command->subcommands[i]; i++) {
			subcommand = command->subcommands[i];
			if (strcmp(subcommand->name, argv[1]) == 0) {
				(*level)++;
				return find_table_command(subcommand,
							  argc - 1, argv + 1, level);
			}
		}
	}
	return command;
}

static int usage_command(struct ccli *ccli, const struct ccli_command_table *command,
			 int argc, char **argv, int level)
{
	int i;

	/* argc is level + 1 if the user did not add more than this command */
	if (argc > level + 1)
		ccli_printf(ccli, "Unknown option: %s\n", argv[level + 1]);

	ccli_printf(ccli, "usage: ");
	for (i = 0; i <= level; i++)
		ccli_printf(ccli, "%s ", argv[i]);

	for (i = 0; command->subcommands[i]; i++)
		ccli_printf(ccli, "%s%s", i ? "|" : "",
			    command->subcommands[i]->name);

	ccli_printf(ccli, "\n");

	return 0;
}

static int command_table_callback(struct ccli *ccli, const char *cmd,
				  const char *line, void *data,
				  int argc, char **argv)
{
	const struct ccli_command_table *command = data;
	int level = 0;

	command = find_table_command(command, argc, argv, &level);

	if (!command->command)
		return usage_command(ccli, command, argc, argv, level);

	data = ccli->command_table_data;

	argc -= level;
	argv += level;

	return command->command(ccli, cmd, line, data, argc, argv);
}

static int test_commands(const struct ccli_command_table *table)
{
	int i;

	if (!table->command && !table->subcommands[0])
		return -1;

	for (i = 0; table->subcommands[i]; i++) {
		if (test_commands(table->subcommands[i]))
			return -1;
	}
	return 0;
}

/**
 * ccli_register_command_table - register a command table
 * @ccli: The CLI descriptor to regsiter a command table to
 * @table: The command table to assign.
 * @data: data that can be passed if the individual commands do not have one.
 */
int ccli_register_command_table(struct ccli *ccli,
				const struct ccli_command_table *table,
				void *data)
{
	const struct ccli_command_table *command;
	bool crashed;
	int i;

	/*
	 * Need to walk the command table to make sure that
	 * every element has populated the "subcommands" field with a
	 * NULL terminated array. Check here to remove unwanted surprises
	 * later. If it crashes, it will report the problem, and not
	 * register the table.
	 */
	crashed = test_for_crash(test_table, table);

	if (crashed) {
		errno = EFAULT;
		return -1;
	}

	/* Make sure all entries have a command */
	if (test_commands(table)) {
		errno = EINVAL;
		return -1;
	}

	ccli->command_table = table;
	ccli->command_table_data = data;

	/* If the top table has a command, the process it only */
	if (table->name && table->command) {
		ccli_register_command(ccli, table->name,
				      command_table_callback, (void *)table);
	}

	/* Add all the commands to process */
	for (i = 0; table->subcommands[i]; i++) {
		command = table->subcommands[i];
		ccli_register_command(ccli, command->name,
				      command_table_callback, (void *)command);
	}
	return 0;
}
