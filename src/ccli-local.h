/* SPDX-License-Identifier: MIT */
#ifndef __CCLI_LOCAL_H
#define __CCLI_LOCAL_H

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <termios.h>

#include <ccli.h>

#define __hidden __attribute__((visibility ("hidden")))

#define ISSPACE(c) isspace((unsigned char)(c))

struct line_buf {
	char *line;
	int size;
	int len;
	int pos;
	int start;	/* used for \ newline */
};

#define DEFAULT_HISTORY_MAX	256
#define DEFAULT_PAGE_SCROLL	24

#define READ_BUF		256

enum {
	CHAR_ERROR		= -1,
	CHAR_INTR		= -2,
	CHAR_IGNORE_END		= -10,
	CHAR_DEL		= -12,
	CHAR_UP			= -13,
	CHAR_DOWN		= -14,
	CHAR_RIGHT		= -15,
	CHAR_LEFT		= -16,
	CHAR_HOME		= -17,
	CHAR_END		= -18,
	CHAR_PAGEUP		= -19,
	CHAR_PAGEDOWN		= -20,
	CHAR_DELWORD		= -21,
	CHAR_RIGHT_WORD		= -22,
	CHAR_LEFT_WORD		= -23,
	CHAR_IGNORE_START_H	= -24,
	CHAR_BACKSPACE		= -25,
	CHAR_REVERSE		= -26,
	CHAR_IGNORE_START	= -27,
	CHAR_INSERT		= -28,
	CHAR_DEL_BEGINNING	= -29,
	/* CHAR_NEWLINE is to tell line_insert() a "\ and newline" was hit */
	CHAR_NEWLINE		= -128,
};

struct command {
	char			*cmd;
	ccli_command_callback	callback;
	ccli_completion		completion;
	void			*data;
};

/* Unused for now */
struct ccli_option {
	const char				*name;
	ccli_option_callback			callback;
};

/* Unused for now */
struct ccli_option_table {
	void					*data;
	struct ccli_option			*options[];
};

struct alias {
	char			*alias;
	char			*command;
	bool			exec;
};

struct ccli {
	struct termios		savein;
	struct termios		saveout;
	struct line_buf		*line;
	char			*temp_line;
	int			history_max;
	int			history_size;
	int			current_line;
	bool			has_alias;
	bool			in_tty;
	int			in;
	int			out;
	int			w_row;
	int			nr_commands;
	int			nr_aliases;
	int			display_index;
	struct command		*commands;
	struct command		enter;
	struct command		unknown;
	struct alias		*aliases;
	const struct ccli_command_table *command_table;
	void			*command_table_data;
	const struct ccli_completion_table *completion_table;
	void			*completion_table_data;
	ccli_completion		default_completion;
	void			*default_completion_data;
	ccli_interrupt		interrupt;
	void			*interrupt_data;
	char			*prompt;
	char			**history;
	char			*delim;
	unsigned char		read_start;
	unsigned char		read_end;
	char			read_buf[READ_BUF];
};

extern int execute(struct ccli *ccli, const char *line, bool hist);

extern void clear_line(struct ccli *ccli, struct line_buf *line);
extern int read_char(struct ccli *ccli);

extern void echo(struct ccli *ccli, char ch);
extern int echo_str(struct ccli *ccli, char *str);
extern void echo_str_len(struct ccli *ccli, char *str, int len);
extern void echo_prompt(struct ccli *ccli);

extern struct command *find_command(struct ccli *ccli, const char *cmd);
extern struct alias *find_alias(struct ccli *ccli, const char *alias);

extern bool check_for_ctrl_c(struct ccli *ccli);
extern char page_stop(struct ccli *ccli);

extern void line_refresh(struct ccli *ccli, struct line_buf *line, int pad);

extern int line_init(struct line_buf *line);
extern int line_init_str(struct line_buf *line, const char *str);
extern void line_reset(struct line_buf *line);
extern void line_cleanup(struct line_buf *line);
extern int line_insert(struct line_buf *line, char ch);
extern bool line_state_escaped(struct line_buf *line);
extern void line_right(struct line_buf *line);
extern void line_left(struct line_buf *line);
extern void line_right_word(struct line_buf *line);
extern void line_left_word(struct line_buf *line);
extern void line_home(struct line_buf *line);
extern void line_end(struct line_buf *line);
extern void line_backspace(struct line_buf *line);
extern void line_del(struct line_buf *line);
extern int line_del_word(struct line_buf *line);
extern int line_del_beginning(struct line_buf *line);
extern int line_copy(struct line_buf *dst, struct line_buf *src, int len);
extern int line_parse(const char *line, char ***pargv,
		      const char *delim, const char **next);
extern void line_replace(struct line_buf *line, char *str);

extern int history_add(struct ccli *ccli, const char *line);
extern int history_up(struct ccli *ccli, struct line_buf *line, int cnt);
extern int history_down(struct ccli *ccli, struct line_buf *line, int cnt);
extern int history_search(struct ccli *ccli, struct line_buf *line, int *pad);
extern void history_free(struct ccli *ccli);

extern void free_argv(int argc, char **argv);

extern void do_completion(struct ccli *ccli, struct line_buf *line, int tab);

extern int exec_alias(struct ccli *ccli, const char *command,
		      const char *line, void *data,
		      int argc, char **argv);

extern int exec_unalias(struct ccli *ccli, const char *command,
			const char *line, void *data,
			int argc, char **argv);
extern int execute_alias(struct ccli *ccli, struct alias *alias,
			 const char *line, int argc, char **argv);

typedef void (*test_crash_callback)(const void *);
extern char test_name[BUFSIZ];
extern char test_message[BUFSIZ];
extern bool test_for_crash(test_crash_callback callback, const void *data);

typedef int (*cache_iter_write_fn)(struct ccli *ccli, int fd, int idx, int cnt, void *data);
typedef int (*cache_iter_read_fn)(struct ccli *ccli, char *line, int ret, int idx, int cnt, void *data);

extern int cache_load_fd(struct ccli *ccli,
			 const char *start_tag, const char *end_tag,
			 const char *tag, int fd, cache_iter_read_fn callback,
			 void *data);

extern int cache_save_fd(struct ccli *ccli, const char *start_tag,
			 const char *end_tag, const char *tag, int fd, int cnt,
			 cache_iter_write_fn callback, void *data);

extern int cache_save_file(struct ccli *ccli,
			   const char *start_tag, const char *end_tag,
			   const char *tag, const char *file, int cnt,
			   cache_iter_write_fn callback, void *data);

extern char *get_cache_file(const char *name);

#endif
