// SPDX-License-Identifier: LGPL-2.1
/*
 * CLI interface for C.
 *
 * Copyright (C) 2022-2023 Steven Rostedt <rostedt@goodmis.org>
 * Copyright (C) 2023 Google, Steven Rostedt <rostedt@goodmis.org>
 */
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>

#include "ccli-local.h"

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
 * cache_save_fd - Write the history into the file descriptor
 * @ccli: The ccli descriptor to write the history of
 * @start_tag: The tag to add in the file at the beginning
 * @end_tag: The tag to add in the file at the end
 * @tag: The tag to give this history segment
 * @fd: The file descriptor to write the history into
 * @cnt: The number of history lines to add
 * @callback: The callback function to call for even cache line.
 * @data: The data to pass to @callback
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
__hidden int cache_save_fd(struct ccli *ccli, const char *start_tag,
			   const char *end_tag, const char *tag, int fd, int cnt,
			   cache_iter_write_fn callback, void *data)
{
	char *str;
	char buf[64];
	int ret;
	int i;

	if (!ccli || !tag || !start_tag || !callback || fd < 0) {
		errno = EINVAL;
		return -1;
	}

	/* Do nothing if there's no cache */
	if (!cnt)
		return 0;

	ret = write(fd, start_tag, strlen(start_tag));
	if (ret < strlen(start_tag))
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

	for (i = 0; i < cnt; i++) {
		if (callback(ccli, fd, i, cnt, data) < 0)
			break;
	}

	ret = write(fd, end_tag, strlen(end_tag));
	if (ret < strlen(end_tag))
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

__hidden int cache_save_file(struct ccli *ccli,
			     const char *start_tag, const char *end_tag,
			     const char *tag, const char *file, int cnt,
			     cache_iter_write_fn callback, void *data)
{
	off_t start, end;
	char *line = NULL;
	int linesz = 0;
	int lines = -1;
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
		lines = has_tag(line, start_tag, tag);
	} while (lines < 0);

	for (i = 0; i < lines; i++) {
		ret = read_line(fd, &line, &linesz);
		if (ret < 0)
			break;
	}
	/* read one more line for end tag */
	read_line(fd, &line, &linesz);
	end = lseek(fd, 0, SEEK_CUR);

	/* Remove this section if found */
	if (lines >= 0)
		remove_section(fd, start, end - start);

	ret = cache_save_fd(ccli, start_tag, end_tag, tag, fd, cnt, callback, data);
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

__hidden int cache_load_fd(struct ccli *ccli,
			   const char *start_tag, const char *end_tag,
			   const char *tag, int fd, cache_iter_read_fn callback,
			   void *data)
{
	char *line = NULL;
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
		cnt = has_tag(line, start_tag, tag);
	} while (cnt < 0);

	for (i = 0; i < cnt; i++) {
		ret = read_line(fd, &line, &linesz);
		if (ret < 0)
			break;
		callback(ccli, line, ret, i, cnt, data);
	}
	if (i < cnt)
		goto out;

	read_line(fd, &line, &linesz);
	/* TODO: test for the end tag. */
out:
	free(line);

	return cnt;
}

__hidden char *get_cache_file(const char *name)
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

	ret = asprintf(&file, "%s/%s", cache_path, name);
	free(cache_path);

	return ret < 0 ? NULL : file;
}
