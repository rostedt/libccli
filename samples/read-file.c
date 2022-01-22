// SPDX-License-Identifier: LGPL-2.1
/*
 * read a binary file via a command line.
 * Copyright (C) 2022 Steven Rostedt <rostedt@goodmis.org>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "ccli.h"

#define RF_PROMPT "rfile> "

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

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

struct rfile {
	void		*map;
	int		fd;
	size_t		pos;
	size_t		size;
	int		len;
};

enum {
	HELP_READ,
	HELP_GOTO,
	HELP_DUMP,
	HELP_ALL,
};

static void show_help(struct ccli *ccli, int type)
{
	switch (type) {
	case HELP_READ:
		ccli_printf(ccli, "To read the current address:\n"
			    "  type '1' or 'x8' for a byte in hex\n"
			    "       'u8' for on unsigned byte\n"
			    "       's8' for a signed byte\n"
			    "  type '2' or 'x16' for short in hex\n"
			    "       'u16' for on unsigned short\n"
			    "       's16' for a signed short\n"
			    "  type '4' or 'x32' for int in hex\n"
			    "       'u32' for on unsigned int\n"
			    "       's32' for a signed int\n"
			    "  type '8' or 'x64' for long long in hex\n"
			    "       'u64' for on unsigned long long\n"
			    "       's64' for a signed long long\n"
			    "  type 'string' followed by optional length\n"
			    "     This will write the string at the location\n");
		break;
	case HELP_GOTO:
		ccli_printf(ccli, "To goto a location in the file:\n"
			    "  type a value to set the offset into the file.\n"
			    "   Add a '+' to add the current position\n"
			    "   Add a '-' to subtract the current position\n");
		break;
	case HELP_DUMP:
		ccli_printf(ccli, "To dump the current location:\n"
			    "  By default, will dump 512 bytes, but if you add\n"
			    "  a length after the command, it will dump that many bytes\n");
		break;
	case HELP_ALL:
		ccli_printf(ccli, "'read' command:\n ");
		show_help(ccli, HELP_READ);
		ccli_printf(ccli, "\n'dump' command:\n ");
		show_help(ccli, HELP_DUMP);
		ccli_printf(ccli, "\n'goto' command:\n ");
		show_help(ccli, HELP_GOTO);
		break;
	}
}

enum {
	TYPE_HEX,
	TYPE_SIGNED,
	TYPE_UNSIGNED,
};

static void show_address(struct rfile *rf, struct ccli *ccli, int offset)
{
	ccli_printf(ccli, "%016zx: ", rf->pos + offset);
}

static int read_string(struct rfile *rf, struct ccli *ccli,
		       int argc, char **argv)
{
	int len;

	if (rf->pos >= rf->size) {
		ccli_printf(ccli, ">> EOF <<\n");
		return 0;
	}

	if (argc)
		len = strtol(argv[0], NULL, 0);
	else
		len = 256;

	if (len < 0) {
		len = rf->size - rf->pos;
		if (len < 0) {
			/* Overflow? */
			len = 1024;
		}
	}

	if (rf->pos + len > rf->size)
		len = rf->size - rf->pos;

	show_address(rf, ccli, 0);
	ccli_printf(ccli, "'%.*s'\n", (int)len, (char *)rf->map + rf->pos);
	return 0;
}

static int read_file(struct ccli *ccli, const char *command,
		     const char *line, void *data,
		     int argc, char **argv)
{
	struct rfile *rf = data;
	const char *type;
	void *ptr;
	long long lval;
	short sval;
	char cval;
	int val;
	int size = -1;
	int t;

	if (argc < 2) {
		show_help(ccli, HELP_READ);
		return 0;
	}

	type = argv[1];

	if (strcmp(type, "string") == 0)
		return read_string(rf, ccli, argc - 2, &argv[2]);

	if (*type == 'x') {
		if (strcmp(type, "x64") == 0)
			type = "8";
		else if (strcmp(type, "x32") == 0)
			type = "4";
		else if (strcmp(type, "x16") == 0)
			type = "2";
		else if (strcmp(type, "x8") == 0)
			type = "1";
	}

	if (isdigit(*type)) {
		t = TYPE_HEX;
		switch (*type) {
		case '8':
			ptr = &lval;
			size = 8;
			break;
		case '4':
			ptr = &val;
			size = 4;
			break;
		case '2':
			ptr = &sval;
			size = 2;
			break;
		case '1':
			ptr = &cval;
			size = 1;
			break;
		}
	}

	if (*type == 's' || *type == 'u') {
		t = *type == 's' ? TYPE_SIGNED : TYPE_UNSIGNED;
		type++;

		if (strcmp(type, "64") == 0) {
			ptr = &lval;
			size = 8;
		} else if (strcmp(type, "32") == 0) {
			ptr = &val;
			size = 4;
		} else if (strcmp(type, "16") == 0) {
			ptr = &sval;
			size = 2;
		} else if (strcmp(type, "8") == 0) {
			ptr = &cval;
			size = 1;
		}
	}

	if (size < 0) {
		ccli_printf(ccli, "Invalid read type '%s'\n", type);
		show_help(ccli, HELP_READ);
		return 0;
	}

	if (rf->pos + size > rf->size) {
		ccli_printf(ccli, "Read will go beyond end of file\n");
		return 0;
	}

	memcpy(ptr, rf->map + rf->pos, size);

	show_address(rf, ccli, 0);

	switch (t) {
	case TYPE_HEX:
		switch (size) {
		case 8:
			ccli_printf(ccli, "0x%08llx\n", lval);
			break;
		case 4:
			ccli_printf(ccli, "0x%08x\n", (unsigned int)val);
			break;
		case 2:
			ccli_printf(ccli, "0x%04x\n", (unsigned int)sval);
			break;
		case 1:
			ccli_printf(ccli, "0x%02x\n", (unsigned char)cval);
			break;
		}
		break;
	case TYPE_UNSIGNED:
		switch (size) {
		case 8:
			ccli_printf(ccli, "%llu\n", lval);
			break;
		case 4:
			ccli_printf(ccli, "%u\n", val);
		case 2:
			ccli_printf(ccli, "%u\n", sval);
		case 1:
			ccli_printf(ccli, "%u\n", cval);
			break;
		}
		break;
	case TYPE_SIGNED:
		switch (size) {
		case 8:
			ccli_printf(ccli, "%lld\n", lval);
			break;
		case 4:
			ccli_printf(ccli, "%d\n", val);
		case 2:
			ccli_printf(ccli, "%d\n", sval);
		case 1:
			ccli_printf(ccli, "%d\n", cval);
			break;
		}
		break;
	}

	/* For repeat operations */
	rf->len = size;

	return 0;
}

enum {
	GOTO_SET,
	GOTO_FORWARD,
	GOTO_BACKWARD,
};

static int read_completion(struct ccli *ccli, const char *command,
			   const char *line, int word, char *match,
			   char ***list, void *data)
{
	char *cmds[] = { "1", "2", "4", "8",
		"x8", "x16", "x32", "x64",
		"s8", "s16", "s32", "s64",
		"u8", "u16", "u32", "u64",
		"string" };
	char **l;
	int cnt;
	int i;

	if (word != 1)
		return 0;

	cnt = ARRAY_SIZE(cmds);

	l = calloc(cnt, sizeof(char *));
	if (!l)
		return -1;

	for (i = 0; i < cnt; i++) {
		l[i] = strdup(cmds[i]);
		if (!l[i])
			goto out_free;
	}

	*list = l;
	return cnt;
 out_free:
	for (; i >= 0; i--)
		free(l[i]);
	free(l);
	return -1;
}

static int goto_file(struct ccli *ccli, const char *command,
		     const char *line, void *data,
		     int argc, char **argv)
{
	struct rfile *rf = data;
	size_t offset;
	char *val;
	int whence = GOTO_SET;

	if (argc < 2) {
		show_help(ccli, HELP_GOTO);
		return 0;
	}

	val = argv[1];
	switch (*val) {
	case '-':
		whence = GOTO_BACKWARD;
		/* fallthrough */
	case '+':
		if (!whence)
			whence = GOTO_FORWARD;
		val++;
		 if (!*val) {
			 /* We allow a space between */
			 if (argc < 3) {
				 show_help(ccli, HELP_GOTO);
				 return 0;
			 }
			 val = argv[2];
		 }
		 break;
	}

	offset = strtoll(val, NULL, 0);

	switch (whence) {
	case GOTO_FORWARD:
		offset += rf->pos;
		break;
	case GOTO_BACKWARD:
		if (offset > rf->pos)
			offset = 0;
		else
			offset = rf->pos - offset;
		break;
	}

	if (offset > rf->size) {
		ccli_printf(ccli,
			   "Size %zx (%zd) is greater than the size of the file (%zd)\n",
			   offset, offset, rf->size);
		return 0;
	}

	rf->pos = offset;

	return 0;
}

static int dump_file(struct ccli *ccli, const char *command,
		     const char *line, void *data,
		     int argc, char **argv)
{
	struct rfile *rf = data;
	unsigned char ch;
	int len = 512;
	int i, x;

	if (argc > 1) {
		len = strtol(argv[1], NULL, 0);
		if (len < 0) {
			ccli_printf(ccli, "Invalid dump length: %d\n", len);
			show_help(ccli, HELP_DUMP);
			return 0;
		}
	}

	/* For repeat operations */
	rf->len = len;

	if (rf->pos + len > rf->size)
		len = rf->size - rf->pos;

	for (i = 0; i < len; i++) {
		show_address(rf, ccli, i);

		for (x = 0; x < 8; x++) {
			if (x + i < len) {
				ch = *(unsigned char *)(rf->map + rf->pos + i + x);
				ccli_printf(ccli, "%02x ", ch);
			} else
				ccli_printf(ccli, "   ");
		}
		ccli_printf(ccli, " ");
		for (x = 0; x < 8; x++) {
			if (x + i + 8 < len) {
				ch = *(unsigned char *)(rf->map + rf->pos + i + x + 8);
				ccli_printf(ccli, "%02x ", ch);
			} else
				ccli_printf(ccli, "   ");
		}
		ccli_printf(ccli, " |");
		for (x = 0; x < 16; x++) {
			if (x + i < len) {
				ch = *(unsigned char *)(rf->map + rf->pos + i + x);
				if (isprint(ch))
					ccli_printf(ccli, "%c", ch);
				else
					ccli_printf(ccli, ".");
			} else
				ccli_printf(ccli, " ");
		}
		ccli_printf(ccli, "|\n");
		i += 15;
	}

	return 0;
}

static void move_forward(struct ccli *ccli, void *data, int len)
{
	char go[64];
	char *goto_argv[] = { "goto", "+", go, NULL };

	snprintf(go, 64, "%d", len);

	goto_file(ccli, "goto", "goto +count", data, 3, goto_argv);
}

static int do_default(struct ccli *ccli, const char *command,
		      const char *line, void *data,
		      int argc, char **argv)
{
	struct rfile *rf = data;
	const char *hist;
	size_t this_pos = rf->pos;

	hist = ccli_history(ccli, 1);
	if (!hist)
		return 0;

	argc = ccli_line_parse(hist, &argv);
	if (argc < 1)
		return 0;

	if (strcmp(argv[0], "read") == 0) {
		move_forward(ccli, data, rf->len);
		if (rf->pos == this_pos)
			goto out;
		read_file(ccli, "read", hist, data, argc, argv);
	}

	else if (strcmp(argv[0], "dump") == 0) {
		move_forward(ccli, data, rf->len);
		if (rf->pos == this_pos)
			goto out;
		dump_file(ccli, "dump", hist, data, argc, argv);
	}

 out:
	ccli_argv_free(argv);
	return 0;
}

static int do_interrupt(struct ccli *ccli, const char *line, int pos, void *data)
{
	/* Do not exit the loop */
	ccli_printf(ccli, "^C\n");
	ccli_line_clear(ccli);
	ccli_printf(ccli, RF_PROMPT);
	return 0;
}

static int do_help(struct ccli *ccli, const char *command,
		   const char *line, void *data,
		   int argc, char **argv)
{
	show_help(ccli, HELP_ALL);
	return 0;
}

static int do_quit(struct ccli *ccli, const char *command,
		   const char *line, void *data,
		   int argc, char **argv)
{
	ccli_printf(ccli, "Goodbye!\n");
	return 1;
}

int main (int argc, char **argv)
{
	struct ccli *cli;
	struct rfile rf;
	struct stat st;
	char *file;

	argv0 = argv[0];

	if (argc < 2)
		usage();

	file = argv[1];

	memset(&rf, 0, sizeof(rf));

	rf.fd = open(file, O_RDONLY);
	if (rf.fd < 0)
		pdie(file);

	if (fstat(rf.fd, &st) < 0)
		pdie("fstat");

	rf.size = st.st_size;

	rf.map = mmap(NULL, rf.size, PROT_READ, MAP_SHARED, rf.fd, 0);
	if (rf.map == MAP_FAILED)
		pdie("mmap");

	printf("Reading file %s\n", file);

	cli = ccli_alloc(RF_PROMPT, STDIN_FILENO, STDOUT_FILENO);
	if (!cli)
		pdie("Creating command line interface");

	ccli_register_command(cli, "read", read_file, &rf);
	ccli_register_command(cli, "goto", goto_file, &rf);
	ccli_register_command(cli, "dump", dump_file, &rf);
	ccli_register_command(cli, "help", do_help, &rf);
	ccli_register_command(cli, "quit", do_quit, &rf);

	ccli_register_completion(cli, "read", read_completion);

	ccli_register_default(cli, do_default, &rf);
	ccli_register_interrupt(cli, do_interrupt, NULL);

	ccli_loop(cli);
	ccli_free(cli);

	exit(0);

	return 0;
}
