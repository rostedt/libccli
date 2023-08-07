// SPDX-License-Identifier: MIT
/*
 * CLI interface for C.
 *
 * Copyright (C) 2022 Steven Rostedt <rostedt@goodmis.org>
 */
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>

#include "ccli-local.h"

#define CCLI_HISTORY_LINE_START \
	"####---ccli---####"

#define CCLI_HISTORY_LINE_END \
	"%%%%---ccli---%%%%"

#define CCLI_NAME "ccli"

static inline int history_idx(struct ccli *ccli, int idx)
{
	return idx % ccli->history_max;
}

__hidden void history_free(struct ccli *ccli)
{
	int max = ccli->history_size;

	if (ccli->history_size > ccli->history_max)
		max = ccli->history_max;

	for (int i = 0; i < max; i++)
		free(ccli->history[i]);

	free(ccli->history);
	ccli->history = NULL;
}

__hidden int history_add(struct ccli *ccli, const char *line)
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

	idx = history_idx(ccli, ccli->history_size);

	free(ccli->history[idx]);
	ccli->history[idx] = strdup(line);
	if (!ccli->history[idx])
		return -1;

	ccli->history_size++;
	ccli->current_line = ccli->history_size;

	return 0;
}

static void save_current(struct ccli *ccli, int current)
{
	char *str;
	int idx;

	/* Store the current line in case it was modifed */
	str = strdup(ccli->line->line);
	if (str) {
		if (current >= ccli->history_size) {
			free(ccli->temp_line);
			ccli->temp_line = str;
		} else {
			idx = history_idx(ccli, current);
			free(ccli->history[idx]);
			ccli->history[idx] = str;
		}
	}
}

__hidden int history_up(struct ccli *ccli, struct line_buf *line, int cnt)
{
	int current = ccli->current_line;
	int idx;

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
	save_current(ccli, current);

	idx = history_idx(ccli, ccli->current_line);
	line_replace(line, ccli->history[idx]);
	return 0;
}

static void restore_current(struct ccli *ccli, struct line_buf *line)
{
	/* Restore the command that was before moving in history */
	if (ccli->temp_line) {
		clear_line(ccli, line);
		line_replace(line, ccli->temp_line);
		free(ccli->temp_line);
		ccli->temp_line = NULL;
	}
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
		restore_current(ccli, line);
		return 1;
	}

	clear_line(ccli, line);

	/* Store the current line in case it was modifed */
	str = strdup(ccli->line->line);
	if (str) {
		idx = history_idx(ccli, current);
		free(ccli->history[idx]);
		ccli->history[idx] = str;
	}

	idx = history_idx(ccli, ccli->current_line);
	line_replace(line, ccli->history[idx]);
	return 0;
}

#define REVERSE_STR	"reverse-i-search"

static void refresh(struct ccli *ccli, struct line_buf *line,
		    struct line_buf *search, int *old_len, const char *p)
{
	int len = 0;
	int pos = 0;
	int i;

	echo_str(ccli, "\r(");
	if (!p)
		len += echo_str(ccli, "failed ");

	len += echo_str(ccli, REVERSE_STR ")`");
	len += echo_str(ccli, search->line);
	len += echo_str(ccli, "': ");
	len += echo_str(ccli, line->line);

	len += line->len;

	if (*old_len > len) {
		pos = *old_len - len;
		for (i = 0; i < pos; i++)
			echo(ccli, ' ');
	}

	pos += line->len - line->pos;
	for (i = 0; i < pos; i++)
		echo(ccli, '\b');

	*old_len = len;
}

__hidden int history_search(struct ccli *ccli, struct line_buf *line, int *pad)
{
	struct line_buf search;
	char *last_hist = NULL;
	char *hist = NULL;
	char *p = NULL;
	int save_current_line = ccli->current_line;
	int old_len;
	int pos = line->pos;
	int min;
	int idx;
	int ret;
	int ch;
	int i;

	*pad = 0;

	if (line_init(&search))
		return CHAR_INTR;

	old_len = line->len + strlen(REVERSE_STR) + 6;

	min = ccli->history_size > ccli->history_max ?
		ccli->history_size - ccli->history_max : 0;

	echo_str(ccli, "\r(" REVERSE_STR ")`': ");
	echo_str(ccli, line->line);
	pos = line->len - pos;
	for (i = 0; i < pos; i++)
		echo(ccli, '\b');

	pos = ccli->current_line;

	for (;;) {
		ch = read_char(ccli);
		switch (ch) {
		case CHAR_INTR:
			echo_str(ccli, "^C\n");
			ccli->current_line = save_current_line;
			line_reset(line);
			line_reset(&search);
			goto out;
		case CHAR_IGNORE_START_H ... CHAR_IGNORE_END:
		case '\n':
		case '\t':
			goto out;
		case CHAR_BACKSPACE:
			if (!search.len)
				break;
			line_backspace(&search);
			goto search;
			break;
		case CHAR_REVERSE:
			pos--;
			last_hist = hist;
			goto search;
		default:
			ret = line_insert(&search, ch);
			if (ret)
				break;
 search:
			p = NULL;
			for (i = pos; i >= min; i--) {
				if (i >= ccli->history_size)
					continue;
				idx = history_idx(ccli, i);
				hist = ccli->history[idx];
				p = strstr(hist, search.line);
				if (!p)
					continue;
				/* Skip duplicates */
				if (last_hist && strcmp(last_hist, hist) == 0)
					continue;
				break;
			}
			if (p) {
				if (ccli->current_line >= ccli->history_size)
					save_current(ccli, ccli->current_line);
				ccli->current_line = i;
				line_replace(line, hist);
				line->pos = p - hist + search.len;
				pos = i;
			}
			refresh(ccli, line, &search, &old_len, p);
			break;
		}
	}
 out:
	*pad = search.len + line->len + sizeof(REVERSE_STR) + 5;
	line_cleanup(&search);
	return ch;
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

static int write_callback(struct ccli *ccli, int fd, int idx, int cnt, void *data)
{
	char *str;
	int ret;

	idx += ccli->history_size - cnt;
	idx = idx % ccli->history_max;
	str = ccli->history[idx];
	ret = write(fd, str, strlen(str));
	if (ret < strlen(str))
		return -1;
	ret = write(fd, "\n", 1);
	if (ret < 1)
		return -1;
	return 0;
}

static int history_cnt(struct ccli *ccli)
{
	int cnt;
	cnt = ccli->history_size;
	if (cnt > ccli->history_max)
		cnt = ccli->history_max;

	return cnt;
}

/**
 * ccli_history_save_fd - Write the history into the file descriptor
 * @ccli: The ccli descriptor to write the history of
 * @tag: The tag to give this history segment
 * @fd: The file descriptor to write the history into
 *
 * Will write the contents of the history into the file descriptor.
 * It will first write a special line that will denote the @tag and
 * size of the history. The @tag is used so that multiple histories
 * can be loaded into the same file, and can be retrieved via the
 * @tag.
 *
 * Returns the number of history lines written on success and -1
 *  on error.
 */
int ccli_history_save_fd(struct ccli *ccli, const char *tag, int fd)
{
	int cnt;

	if (!ccli || !tag || fd < 0) {
		errno = EINVAL;
		return -1;
	}

	cnt = history_cnt(ccli);

	/* Do nothing if there's no history */
	if (!cnt)
		return 0;

	return cache_save_fd(ccli, CCLI_HISTORY_LINE_START,
			     CCLI_HISTORY_LINE_END, tag, fd, cnt,
			     write_callback, NULL);
}

static int read_callback(struct ccli *ccli, char *line, int ret, int idx,
			 int cnt, void *data)
{
	const char *p;

	/* Do not add empty lines */
	if (!ret)
		return 0;

	/* Do not add "exit" if that was last item */
	if (idx == cnt - 1 && strncmp(line, "exit", 4) == 0) {
		for (p = line + 4; isspace(*p); p++)
			;
		if (!*p)
			return 0;
	}
	history_add(ccli, line);

	return 0;
}

/**
 * ccli_history_load_fd - Read the history into the file descriptor
 * @ccli: The ccli descriptor to read the history from
 * @tag: The tag to use to find in the file
 * @fd: The file descriptor to read the history from
 *
 * Will read the file descriptor looking for the start of history
 * that matches the @tag. Then it will load the found history into
 * the @ccli history.
 *
 * Returns the number of history lines read on success and -1
 *  on error.
 */
int ccli_history_load_fd(struct ccli *ccli, const char *tag, int fd)
{
	int ret;

	if (!ccli || !tag || fd < 0) {
		errno = EINVAL;
		return -1;
	}

	ret = cache_load_fd(ccli, CCLI_HISTORY_LINE_START,
			    CCLI_HISTORY_LINE_END, tag, fd, read_callback, NULL);

	return ret;
}

/**
 * ccli_history_save_file - Write the history into a file
 * @ccli: The ccli descriptor to write the history of
 * @tag: The tag to give this history segment
 * @file: The file path to write to
 *
 * It will first search for the @tag, and if it is there, it will
 * remove the contents of that @tag and replace it with the
 * new content.
 * Will write the contents of the history into the @file path.
 * It will first write a special line that will denote the @tag and
 * size of the history. The @tag is used so that multiple histories
 * can be loaded into the same file, and can be retrieved via the
 * @tag.
 *
 * Returns the number of history lines written on success and -1
 *  on error.
 */
int ccli_history_save_file(struct ccli *ccli, const char *tag, const char *file)
{
	int cnt;

	cnt = history_cnt(ccli);

	/* Do nothing if there's no history */
	if (!cnt)
		return 0;

	return cache_save_file(ccli, CCLI_HISTORY_LINE_START,
			       CCLI_HISTORY_LINE_END, tag, file, cnt,
			       write_callback, NULL);
}

/**
 * ccli_history_load_file - Read the history from the given file path
 * @ccli: The ccli descriptor to read the history from
 * @tag: The tag to use to find in the file
 * @file: The file path to read the history from
 *
 * Will read the @file looking for the start of history
 * that matches the @tag. Then it will load the found history into
 * the @ccli history.
 *
 * Returns the number of history lines read on success and -1
 *  on error.
 */
int ccli_history_load_file(struct ccli *ccli, const char *tag, const char *file)
{
	int ret;
	int fd;

	fd = open(file, O_RDONLY);
	if (fd < 0)
		return -1;

	ret = ccli_history_load_fd(ccli, tag, fd);

	close(fd);
	return ret;
}

/**
 * ccli_history_save - Write the history into the default ccli file
 * @ccli: The ccli descriptor to write the history of
 * @tag: The tag to give this history segment
 *
 * Will write the contents of the history into the the default
 * ccli file that is located in $(XDG_CACHE_HOME)/ccli or in
 * $(HOME)/.cache/ccli.
 *
 * It will first write a special line that will denote the @tag and
 * size of the history. The @tag is used so that multiple histories
 * can be loaded into the same file, and can be retrieved via the
 * @tag.
 *
 * Returns the number of history lines written on success and -1
 *  on error.
 */
int ccli_history_save(struct ccli *ccli, const char *tag)
{
	char *file;
	int ret;

	file = get_cache_file(CCLI_NAME);
	ret = ccli_history_save_file(ccli, tag, file);
	free(file);

	return ret;
}

/**
 * ccli_history_load - Read the history from the default path
 * @ccli: The ccli descriptor to read the history from
 * @tag: The tag to use to find in the file
 *
 * Will read the default path defined by either $XDG_CACHE_HOME/ccli or
 * $HOME/.cache/ccli and then look for the history
 * that matches the @tag. Then it will load the found history into
 * the @ccli history.
 *
 * Returns the number of history lines read on success and -1
 *  on error.
 */
int ccli_history_load(struct ccli *ccli, const char *tag)
{
	char *file;
	int ret;

	file = get_cache_file(CCLI_NAME);
	ret = ccli_history_load_file(ccli, tag, file);
	free(file);

	return ret;
}
