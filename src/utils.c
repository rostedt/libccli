// SPDX-License-Identifier: LGPL-2.1
/*
 * Copyright (C) 2023 Google Inc, Steven Rostedt <rostedt@goodmis.org>
 */
#include <sys/ioctl.h>
#include <signal.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>

#include "ccli-local.h"

__hidden char test_name[BUFSIZ];
__hidden char test_message[BUFSIZ];

static sigjmp_buf env;
static void handler(int sig, siginfo_t *info, void *context)
{
	fprintf(stderr, "ccli: %s\n", test_message);
	fprintf(stderr, "      While processing '%s'\n", test_name);

	siglongjmp(env, 1);
}

/**
 * test_for_crash - call a function and report if a SIGSEV is detected
 * @callback: The callback to test
 *
 * Run a callback function and return if it triggers a SIGSEV or not.
 */
__hidden bool test_for_crash(test_crash_callback callback, const void *data)
{
	struct sigaction act = { 0 };
	struct sigaction oldact;
	bool crashed = false;

	memset(test_name, 0, sizeof(test_name));

	/* Set the signal for SIGSEGV and save the old handler */
	act.sa_flags = SA_ONSTACK | SA_SIGINFO;
	sigemptyset(&act.sa_mask);
	act.sa_sigaction = &handler;

	if (sigaction(SIGSEGV, &act, &oldact) < 0)
		return -1;

	/* On fault, it will jump back here and skip calling the callback again */
	if (sigsetjmp(env, 0) == 1) {
		crashed = true;
	} else {
		/* If this faults, it will go to the above if block */
		callback(data);
	}

	sigaction(SIGSEGV, &oldact, NULL);

	return crashed;
}

