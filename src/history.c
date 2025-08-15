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

static inline int history_idx(struct ccli *ccli, int idx)
{
	return idx % ccli->history_max;
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
	char *str = CCLI_HISTORY_LINE_START;
	char buf[64];
	int cnt;
	int ret;
	int idx;
	int i;

	if (!ccli || !tag || fd < 0) {
		errno = EINVAL;
		return -1;
	}

	cnt = ccli->history_size;
	if (cnt > ccli->history_max)
		cnt = ccli->history_max;

	/* Do nothing if there's no history */
	if (!cnt)
		return 0;

	ret = write(fd, str, strlen(str));
	if (ret < strlen(str))
		return -1;

	str = " ";
	ret = write(fd, str, 1);
	if (ret != 1)
		return -1;

	ret = write(fd, tag, strlen(tag));
	if (ret < strlen(tag))
		return -1;

	snprintf(buf, 64, " %d\n", cnt);
	ret = write(fd, buf, strlen(buf));
	if (ret < strlen(buf))
		return -1;

	idx = ccli->history_size - cnt;

	for (i = 0; i < cnt; i++, idx++) {
		idx = idx % ccli->history_max;
		str = ccli->history[idx];
		ret = write(fd, str, strlen(str));
		if (ret < strlen(str))
			return -1;
		ret = write(fd, "\n", 1);
		if (ret < 1)
			return -1;
	}

	str = CCLI_HISTORY_LINE_END;

	ret = write(fd, str, strlen(str));
	if (ret < strlen(str))
		return -1;

	str = " ";
	ret = write(fd, str, 1);
	if (ret != 1)
		return -1;

	ret = write(fd, tag, strlen(tag));
	if (ret < strlen(tag))
		return -1;

	str = "\n";
	ret = write(fd, str, 1);
	if (ret < 1)
		return -1;

	return cnt;
}

static char *update_line(char *line, int *linesz, int cnt)
{
	char *tmp;

	if (*linesz <= (cnt + 1)) {
		tmp = realloc(line, cnt + BUFSIZ);
		if (!tmp) {
			free(line);
			return NULL;
		}
		line = tmp;
		*linesz = cnt + BUFSIZ;
	}
	return line;
}

static int read_bytes(int fd, char **pline, int *linesz)
{
	char *line = *pline;
	char ch;
	int cnt = 0;
	int r;

	while ((r = read(fd, &ch, 1)) < 1) {
		if (ch == '\n')
			break;
		line = update_line(line, linesz, cnt);
		if (!line)
			return -1;
		line[cnt++] = ch;
	}
	if (!cnt)
		return r > 0 ? 0 : -1;

	if (r < 0)
		return -1;
	if (cnt)
		line[cnt] = '\0';
	*pline = line;
	return cnt;
}

static int read_line(int fd, char **pline, int *linesz)
{
	char buf[BUFSIZ+1];
	char *line;
	char *p;
	off64_t offset;
	int cnt = 0;
	int len;
	int r;

	/* Make sure line is allocated */
	*pline = update_line(*pline, linesz, 0);
	line = *pline;

	offset = lseek64(fd, 0, SEEK_CUR);
	if (offset < 0) {
		/*
		 * The fd could be a pipe or a stream, and we do not want
		 * to read any more than we have to. So we are stuck with
		 * reading one byte at a time.
		 */
		return read_bytes(fd, pline, linesz);
	}

	while ((r = read(fd, buf, BUFSIZ)) > 0) {
		buf[r] = '\0';
		p = strchr(buf, '\n');

		len = p ? p - buf : r;

		line = update_line(line, linesz, cnt + len);
		if (!line)
			return -1;
		memcpy(line + cnt, buf, len);
		cnt += len;
		line[cnt] = '\0';

		if (p)
			break;
	}
	if (!cnt)
		return r ? 0 : -1;

	/* Put back to after the new line (cnt is at new line) */
	offset = lseek64(fd, offset + cnt + 1, SEEK_SET);
	// What should we do if this fails?

	*pline = line;
	return cnt;
}

static int has_tag(const char *line, const char *str, const char *tag)
{
	int taglen;
	char *p;

	taglen = strlen(tag);
	p = strstr(line, tag);
	if (!p)
		return -1
;
	if (p[taglen] != ' ')
		return -1;

	for (p += taglen; isspace(*p); p++)
		;

	return atoi(p);
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
	char *str = CCLI_HISTORY_LINE_START;
	char *line = NULL;
	char *p;
	int linesz = 0;
	int cnt;
	int ret;
	int i;

	if (!ccli || !tag || fd < 0) {
		errno = EINVAL;
		return -1;
	}

	do {
		ret = read_line(fd, &line, &linesz);
		if (ret < 0)
			break;
		cnt = has_tag(line, str, tag);
	} while (cnt < 0);

	for (i = 0; i < cnt; i++) {
		ret = read_line(fd, &line, &linesz);
		if (ret < 0)
			break;
		/* Do not add empty lines */
		if (!ret)
			continue;
		/* Do not add "exit" if that was last item */
		if (i == cnt - 1 && strncmp(line, "exit", 4) == 0) {
			for (p = line + 4; isspace(*p); p++)
				;
			if (!*p)
				continue;
		}
		history_add(ccli, line);
	}
	if (i < cnt)
		goto out;

	read_line(fd, &line, &linesz);
	/* TODO: test for the end tag. */
out:
	free(line);

	return cnt;
}

static int remove_section(int fd, off_t start, off_t size)
{
	char buf[BUFSIZ];
	off_t roffset;
	off_t offset;
	int r;

	offset = lseek(fd, start, SEEK_SET);
	if (offset < 0)
		return -1;

	roffset = lseek64(fd, size, SEEK_CUR);
	do {
		r = read(fd, buf, BUFSIZ);
		if (r < 0)
			return -1;
		offset = lseek(fd, offset, SEEK_SET);
		if (offset < 0)
			return -1;
		offset += r;
		write(fd, buf, r);
		roffset = lseek(fd, roffset + r, SEEK_SET);
		if (roffset < 0)
			return -1;
	} while (r);

	/* Set to the end of the new file */
	lseek(fd, offset, SEEK_SET);
	return 0;
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
	char *str = CCLI_HISTORY_LINE_START;
	off_t start, end;
	char *line = NULL;
	int linesz = 0;
	int cnt = -1;
	int ret;
	int fd;
	int i;

	fd = open(file, O_RDWR | O_CREAT, 0640);
	if (fd < 0)
		return -1;

	/* First remove the current tag */
	do {
		start = lseek(fd, 0, SEEK_CUR);
		ret = read_line(fd, &line, &linesz);
		if (ret < 0)
			break;
		cnt = has_tag(line, str, tag);
	} while (cnt < 0);

	for (i = 0; i < cnt; i++) {
		ret = read_line(fd, &line, &linesz);
		if (ret < 0)
			break;
	}
	/* read one more line for end tag */
	read_line(fd, &line, &linesz);
	end = lseek(fd, 0, SEEK_CUR);

	/* Remove this section if found */
	if (cnt >= 0)
		remove_section(fd, start, end - start);

	ret = ccli_history_save_fd(ccli, tag, fd);
	if (ret < 0)
		goto out;

	/* In case what we wrote is less than what we removed */
	end = lseek(fd, 0, SEEK_CUR);
	ftruncate(fd, end);
out:
	free(line);
	close(fd);
	return ret;
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

static char *get_cache_file(void)
{
	char *cache_path;
	char *home;
	char *file;
	int ret;

	cache_path = secure_getenv("XDG_CACHE_HOME");
	if (!cache_path) {
		home = secure_getenv("HOME");
		if (!home)
			return NULL;
		ret = asprintf(&cache_path, "%s/.cache", home);
		if (ret < 0)
			return NULL;
	} else {
		/* So we can always free it later */
		cache_path = strdup(cache_path);
	}
	if (!cache_path)
		return NULL;

	ret = asprintf(&file, "%s/ccli", cache_path);
	free(cache_path);

	return ret < 0 ? NULL : file;
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

	file = get_cache_file();
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

	file = get_cache_file();
	ret = ccli_history_load_file(ccli, tag, file);
	free(file);

	return ret;
}
