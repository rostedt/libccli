// SPDX-License-Identifier: MIT
/*
 * Copyright (C) 2022, Steven Rostedt <rostedt@goodmis.org>
 *  Based off of the libtracefs utests that are:
 *    Copyright (C) 2020, VMware, Tzvetomir Stoyanov <tz.stoyanov@gmail.com>
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <ctype.h>

#include <CUnit/CUnit.h>
#include <CUnit/Basic.h>

#include "ccli.h"

#define CCLI_SUITE		"ccli library"
#define TEST_INSTANCE_NAME	"cunit_test_iter"

#define CCLI_PROMPT		"test> "
#define CCLI_MAGIC		"MAGIC"
#define CCLI_RUN_COMPLETE	"run completed"
#define CCLI_SHOW_COMPLETE	"show completed"

struct test_ccli_connect {
	char			magic[sizeof(CCLI_MAGIC)];
	struct ccli		*ccli;
	const char		*line;
	const char		**words;
	int			nr_words;
	int			ccli_in;
	int			ccli_out;
	int			cons_in;
	int			cons_out;
	int			ret;
	bool			run;
};

static struct test_ccli_connect ccli_connect;
static pthread_t cons_thread;

static pthread_barrier_t pbarrier;

static void wait_for_console(void)
{
	pthread_barrier_wait(&pbarrier);
}

static void little_pause(void)
{
	struct timespec ts;

	ts.tv_sec = 0;
	ts.tv_nsec = 250 * 1000 * 1000;

	printf("(pause)...");
	fflush(stdout);
	nanosleep(&ts, NULL);
}

__attribute__((__format__(printf, 1, 2)))
static void write_ccli(const char *fmt, ...)
{
	va_list ap;
	char buf[BUFSIZ];
	int len;
	int out;
	int w;

	out = ccli_connect.ccli_out;

	va_start(ap, fmt);
	len = vsnprintf(buf, BUFSIZ, fmt, ap);
	va_end(ap);

	w = write(out, buf, len);
	CU_TEST(w == len);
	if (buf[len - 1] == '\n')
		wait_for_console();
	else
		little_pause();
}

static void dump_line(const char *line, int len)
{
	unsigned char ch;
	int save_len = len;
	int offset = 0;
	int i;

	for (i = 0; i < 8; i++, offset++) {
		if (len) {
			ch = line[offset];
			printf("%02x ", ch);
			len--;
		} else
			printf("   ");
	}

	printf(" ");
	for (i = 0; i < 8; i++, offset++) {
		if (len) {
			ch = line[offset];
			printf("%02x ", ch);
			len--;
		} else
			printf("   ");
	}

	printf(" |");

	offset -= 16;
	len = save_len;

	for (i = 0; i < 16; i++, offset++) {
		if (len) {
			ch = line[offset];
			if (isprint(ch))
				printf("%c", ch);
			else
				printf(".");
			len--;
		} else
				printf(" ");
	}
	printf("|\n");
}

static void dump_lines(const char *line, int len)
{
	while (len) {
		dump_line(line, len);
		line += 16;
		if (len > 16)
			len -= 16;
		else
			len = 0;
	}
}

static bool do_read(const char *match, bool exact)
{
	char buffer[BUFSIZ + 1];
	char buf[BUFSIZ + 1];
	int in;
	int r, x = 0, s = 0;
	bool err = false;

	in = ccli_connect.ccli_in;

	buffer[0] = '\0';
	r = read(in, buffer, BUFSIZ);
	buffer[r] = '\0';
	for (int i = 0; i < r; i++) {
		buf[x] = buffer[i];
		switch (buffer[i]) {
		case '\n':
			x++;
			s = x;
			break;
		case '\r':
			x = s;
			break;
		case '\b':
			CU_TEST(x > 0);
			err = err || x <= 0;
			x--;
			if (x < s)
				x = s;
			break;
		default:
			x++;
		}
	}
	buf[x] = '\0';
	r = strlen(buf);

	if (exact) {
		CU_TEST(r == strlen(match));
		if (r != strlen(match)) {
			printf("\nbuf:'%s'  match:'%s'\n", buf, match);
			printf("r:%d mlen:%ld\n", r , strlen(match));
			dump_lines(buf, r);
			err = true;
		}
		CU_TEST(strncmp(buf, match, strlen(match)) == 0);
		if (strncmp(buf, match, strlen(match)) != 0)
			err = true;
	} else {
		CU_TEST(strstr(buf, match) != NULL);
		if (!strstr(buf, match)) {
			printf("\nmatch:'%s' not in buf:'%s'\n", match, buf);
			printf("\n");
			dump_lines(buf, r);
			err = true;
		}
	}
	return err;
}

/* Defined as a macro to show where it may have failed */
#define read_ccli(match, exact)						\
	do {								\
		CU_TEST(!do_read(match, exact));			\
	} while (0)

static int create_ccli(const char *prompt)
{
	int brass_in[2];
	int brass_out[2];
	int ret;

	ret = pipe(brass_in);
	if (ret < 0)
		return -1;
	ret = pipe(brass_out);
	if (ret < 0)
		return -1;

	ccli_connect.ccli_in = brass_out[0];
	ccli_connect.ccli_out = brass_in[1];

	ccli_connect.cons_in = brass_in[0];
	ccli_connect.cons_out = brass_out[1];

	ccli_connect.ccli = ccli_alloc(prompt,
				       brass_in[0], brass_out[1]);

	CU_TEST(ccli_connect.ccli != NULL);
	if (!ccli_connect.ccli)
		return -1;
	return 0;
}

static void destroy_ccli(void)
{
	ccli_free(ccli_connect.ccli);
	ccli_connect.ccli = NULL;

	close(ccli_connect.ccli_in);
	close(ccli_connect.ccli_out);
	close(ccli_connect.cons_in);
	close(ccli_connect.cons_out);
}

static void __execute_command(const char *line, const char *words[], int nr_words)
{
	ccli_connect.line = line;
	ccli_connect.nr_words = nr_words;
	ccli_connect.words = words;
	write_ccli(ccli_connect.line);
}

#define WORDS(x...)	{ x }
#define execute_command(line, words, nr_words)				\
	do {								\
		const char *word_list[] = words;			\
		__execute_command(line, word_list, nr_words);		\
	} while (0)

static void test_ccli_exit(void)
{
	if (create_ccli(CCLI_PROMPT) < 0)
		return;

	/* start the console */
	wait_for_console();

	read_ccli(CCLI_PROMPT, true);

	ccli_connect.ret = -1;

	write_ccli("exit\n");
	read_ccli("exit", false);

	CU_TEST(!ccli_connect.ret);

	destroy_ccli();

	return;
}

static int command_run(struct ccli *ccli, const char *command,
		       const char *line, void *data,
		       int argc, char **argv)
{
	struct test_ccli_connect *conn = data;
	char buf[BUFSIZ];
	int i;

	CU_TEST(strcmp(conn->magic, CCLI_MAGIC) == 0);

	/* Subtract the '\n' from conn->line */
	strncpy(buf, conn->line, BUFSIZ);
	buf[strlen(buf) - 1] = '\0';

	CU_TEST(strcmp(buf, line) == 0);
	CU_TEST(argc == conn->nr_words);
	for (i = 0; i < argc; i++) {
		CU_TEST(conn->words[i] != NULL);
		if (conn->words[i] == NULL)
			break;
		CU_TEST(strcmp(conn->words[i], argv[i]) == 0);
	}

	ccli_printf(ccli, "%s\n", CCLI_RUN_COMPLETE);
	wait_for_console();

	return 0;
}

static int command_show(struct ccli *ccli, const char *command,
			const char *line, void *data,
			int argc, char **argv)
{
	ccli_printf(ccli, "%s\n", CCLI_SHOW_COMPLETE);
	wait_for_console();
	return 0;
}

static int register_commands(struct ccli *ccli)
{
	int r;

	r = ccli_register_command(ccli, "run", command_run, &ccli_connect);
	CU_TEST(!r);
	if (r)
		return r;

	r = ccli_register_command(ccli, "show", command_show, &ccli_connect);
	CU_TEST(!r);

	return r;
}

static void test_ccli_command(void)
{
	struct ccli *ccli;
	int r;

	if (create_ccli(CCLI_PROMPT) < 0)
		return;

	ccli = ccli_connect.ccli;

	r = register_commands(ccli);
	if (r)
		return;

	wait_for_console();


	read_ccli(CCLI_PROMPT, true);

	execute_command("run\n", WORDS("run"), 1);
	read_ccli(CCLI_RUN_COMPLETE, false);

	execute_command("run  for you\\'r 'life\\!'\n",
			WORDS("run", "for", "you'r", "life!"), 4);
	read_ccli(CCLI_RUN_COMPLETE, false);

	write_ccli("exit\n");
	destroy_ccli();

	return;
}

static void test_ccli_completion(void)
{
	struct ccli *ccli;
	int r;

	if (create_ccli(CCLI_PROMPT) < 0)
		return;

	ccli = ccli_connect.ccli;

	r = register_commands(ccli);
	if (r)
		return;

	wait_for_console();

	read_ccli(CCLI_PROMPT, true);

	write_ccli("sh\t\t");
	read_ccli(CCLI_PROMPT "show ", true);

	write_ccli("\n");
	read_ccli(CCLI_SHOW_COMPLETE, false);

	write_ccli("exit\n");
	destroy_ccli();

	return;
}

static int test_suite_destroy(void)
{
	void *cret;

	ccli_connect.run = false;
	wait_for_console();
	pthread_join(cons_thread, &cret);
	ccli_free(ccli_connect.ccli);
	return 0;
}

static void *ccli_console(void *data)
{
	struct test_ccli_connect *conn = data;

	printf("enter loop\n");
	pthread_barrier_wait(&pbarrier);
	do {
		pthread_barrier_wait(&pbarrier);
		if (!conn->run)
			break;
		printf("start loop...");
		fflush(stdout);
		conn->ret = ccli_loop(conn->ccli);
		printf("end loop...");
		fflush(stdout);
		pthread_barrier_wait(&pbarrier);
	} while (conn->run);

	printf("\n\nexit loop\n");
	return NULL;
}

static int test_suite_init(void)
{
	int ret;

	ccli_connect.run = true;

	strcpy(ccli_connect.magic, CCLI_MAGIC);

	ret = pthread_barrier_init(&pbarrier, NULL, 2);
	if (ret < 0)
		goto err_free;

	ret = pthread_create(&cons_thread, NULL, ccli_console, &ccli_connect);
	if (ret < 0)
		goto err_free;
	wait_for_console();
	return 0;
 err_free:
	ccli_free(ccli_connect.ccli);
	return 1;
}

void test_ccli_lib(void)
{
	CU_pSuite suite = NULL;

	suite = CU_add_suite(CCLI_SUITE, test_suite_init, test_suite_destroy);
	if (suite == NULL) {
		fprintf(stderr, "Suite \"%s\" cannot be ceated\n", CCLI_SUITE);
		return;
	}
	CU_add_test(suite, "ccli exit",
		    test_ccli_exit);
	CU_add_test(suite, "ccli command",
		    test_ccli_command);
	CU_add_test(suite, "ccli completion",
		    test_ccli_completion);
}
