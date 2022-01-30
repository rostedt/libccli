// SPDX-License-Identifier: LGPL-2.1
/*
 * CLI interface for C.
 *
 * Copyright (C) 2022 Steven Rostedt <rostedt@goodmis.org>
 */

#include "ccli-local.h"

__hidden int history_add(struct ccli *ccli, char *line)
{
	char **lines;
	int idx;

	if (ccli->history_size < ccli->history_max) {
		lines = realloc(ccli->history, sizeof(*lines) * (ccli->history_size + 1));
		if (!lines)
			return -1;
		lines[ccli->history_size] = NULL;
		ccli->history = lines;
	}

	idx = ccli->history_size % ccli->history_max;

	free(ccli->history[idx]);
	ccli->history[idx] = strdup(line);
	if (!ccli->history[idx])
		return -1;

	ccli->history_size++;
	ccli->current_line = ccli->history_size;

	return 0;
}

__hidden int history_up(struct ccli *ccli, struct line_buf *line, int cnt)
{
	int current = ccli->current_line;
	int idx;
	char *str;

	if (ccli->current_line > cnt)
		ccli->current_line -= cnt;
	else
		ccli->current_line = 0;

	if (ccli->history_size > ccli->history_max &&
	    ccli->current_line <= ccli->history_size - ccli->history_max)
		ccli->current_line = (ccli->history_size - ccli->history_max) + 1;

	if (current == ccli->current_line)
		return 1;

	clear_line(ccli, line);

	/* Store the current line in case it was modifed */
	str = strdup(ccli->line->line);
	if (str) {
		if (current >= ccli->history_size) {
			free(ccli->temp_line);
			ccli->temp_line = str;
		} else {
			idx = current % ccli->history_max;
			free(ccli->history[idx]);
			ccli->history[idx] = str;
		}
	}

	idx = ccli->current_line % ccli->history_max;
	line_replace(line, ccli->history[idx]);
	return 0;
}

__hidden int history_down(struct ccli *ccli, struct line_buf *line, int cnt)
{
	int current = ccli->current_line;
	int idx;
	char *str;

	ccli->current_line += cnt;

	if (ccli->current_line > (ccli->history_size))
		ccli->current_line = ccli->history_size;

	if (ccli->current_line == ccli->history_size) {
		/* Restore the command that was before moving in history */
		if (ccli->temp_line) {
			clear_line(ccli, line);
			line_replace(line, ccli->temp_line);
			free(ccli->temp_line);
			ccli->temp_line = NULL;
		}
		return 1;
	}

	clear_line(ccli, line);

	/* Store the current line in case it was modifed */
	str = strdup(ccli->line->line);
	if (str) {
		idx = current % ccli->history_max;
		free(ccli->history[idx]);
		ccli->history[idx] = str;
	}

	idx = ccli->current_line % ccli->history_max;
	line_replace(line, ccli->history[idx]);
	return 0;
}

/**
 * ccli_history - return a previous entered line from the past
 * @ccli: The ccli descriptor to read the histor from
 * @past: How far back to go
 *
 * Reads the line that happened @past commands ago and returns it.
 *
 * Returns a string that should not be modifiied if there was
 *   a command that happened @past commands ago, otherwise NULL.
 */
const char *ccli_history(struct ccli *ccli, int past)
{
	int idx;

	if (past > ccli->history_size ||
	    past > ccli->history_max)
		return NULL;

	idx = ccli->history_size - past;
	idx %= ccli->history_max;

	return ccli->history[idx];
}
