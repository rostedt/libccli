# This is a very simple shell like interface for CLI activities.

More will be added to this, but for now, this is the basic
idea:

```
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ccli.h>

static int say_hello(struct ccli *ccli, const char *command,
		     const char *line, void *data,
		     int argc, char **argv)
{
	char *this = (char *)data;
	int i;

	/* command matches argv[0] */
	/* line is the full line that was typed */
	/* argc is the number of words in the line */
	/* argv is the individual words */

	ccli_printf(ccli, "You typed %s\n", line);
	ccli_printf(ccli, "which is broken up into:\n");
	for (i = 0; i < argc; i++)
		ccli_printf(ccli, "word %d:\t%s\n", i, argv[i]);

	ccli_printf(ccli, "And this is passed %s\n", this);

	ccli_printf(ccli, "Type 'exit' to quit\n");

	/* returning anything but zero will exit the loop */
	/* By default, ccli will add the "exit" command to exit the loop */
	return 0;
}

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

static int hello_completion(struct ccli *ccli, const char *command,
			    const char *line, int word, const char *match,
			    char ***list, void *data)
{
	const char *word1[] = {
		"Sir", "Madam", "you", "kitty", "is", "for", "my",
		"good", "Mr." };
	const char *word2[] = {
		"Darling", "there", "now", "Hunk", "Bond" };
	const char *word3[] = { "anybody", "I" };
	const char *word4[] = { "out", "want" };
	const char *word5[] = { "there", "you" };
	const char **words;
	char **l;
	int len;
	int i;

	switch(word) {
	case 1:
		words = word1;
		len = ARRAY_SIZE(word1);
		break;
	case 2:
		words = word2;
		len = ARRAY_SIZE(word2);
		break;
	case 3:
		words = word3;
		len = ARRAY_SIZE(word3);
		break;
	case 4:
		words = word4;
		len = ARRAY_SIZE(word4);
		break;
	case 5:
		words = word5;
		len = ARRAY_SIZE(word5);
		break;
	default:
		return -1;
	}

	/*
	 * We do not worry about matching, the caller will only display
	 * words that do match "match".
	 */
	l = calloc(len, sizeof(char *));
	if (!l)
		return -1;

	for (i = 0; i < len; i++) {
		l[i] = strdup(words[i]);
		/*
		 * If the above fails to alloc, the caller will just
		 * ignore this field. It will be treated as a non-match.
		 * This may be an issues for the user, but if there's
		 * memory allocation issues, the user probably has more
		 * to worry about.
		 */
	}

	*list = l;
	return len;
}

int main(int argc, char **argv)
{
	struct ccli *ccli;
	char *this_data = "something here";

	ccli = ccli_alloc("myprompt> ", STDIN_FILENO, STDOUT_FILENO);

	ccli_register_command(ccli, "hello", say_hello, this_data);

	ccli_register_completion(ccli, "hello", hello_completion);

	ccli_loop(ccli);

	ccli_free(ccli);

	return 0;
}
```

## To build:

```
  make
```
## To build the sample file(s)

```
  make samples
```

The samples will be placed in a local bin directory. One that you may find
useful is the read-file sample.

```
 bin/read-file binary-file
```

This will give you a command line interface that can read the binary file:

```
-----------------------------------------------------------------------------
 $ bin/read-file trace.dat
Reading file /tmp/trace.dat
rfile> read 8
0000000000000000: 0x6963617274440817
rfile> goto 0x100
rfile> dump 256
0000000000000100: cd 00 00 00 00 00 00 00  23 20 63 6f 6d 70 72 65  |........# compre|
0000000000000110: 73 73 65 64 20 65 6e 74  72 79 20 68 65 61 64 65  |ssed entry heade|
0000000000000120: 72 0a 09 74 79 70 65 5f  6c 65 6e 20 20 20 20 3a  |r..type_len    :|
0000000000000130: 20 20 20 20 35 20 62 69  74 73 0a 09 74 69 6d 65  |    5 bits..time|
0000000000000140: 5f 64 65 6c 74 61 20 20  3a 20 20 20 32 37 20 62  |_delta  :   27 b|
0000000000000150: 69 74 73 0a 09 61 72 72  61 79 20 20 20 20 20 20  |its..array      |
0000000000000160: 20 3a 20 20 20 33 32 20  62 69 74 73 0a 0a 09 70  | :   32 bits...p|
0000000000000170: 61 64 64 69 6e 67 20 20  20 20 20 3a 20 74 79 70  |adding     : typ|
0000000000000180: 65 20 3d 3d 20 32 39 0a  09 74 69 6d 65 5f 65 78  |e == 29..time_ex|
0000000000000190: 74 65 6e 64 20 3a 20 74  79 70 65 20 3d 3d 20 33  |tend : type == 3|
00000000000001a0: 30 0a 09 74 69 6d 65 5f  73 74 61 6d 70 20 3a 20  |0..time_stamp : |
00000000000001b0: 74 79 70 65 20 3d 3d 20  33 31 0a 09 64 61 74 61  |type == 31..data|
00000000000001c0: 20 6d 61 78 20 74 79 70  65 5f 6c 65 6e 20 20 3d  | max type_len  =|
00000000000001d0: 3d 20 32 38 0a 12 00 00  00 55 03 00 00 00 00 00  |= 28.....U......|
00000000000001e0: 00 6e 61 6d 65 3a 20 77  61 6b 65 75 70 0a 49 44  |.name: wakeup.ID|
00000000000001f0: 3a 20 33 0a 66 6f 72 6d  61 74 3a 0a 09 66 69 65  |: 3.format:..fie|
rfile> goto + 8
rfile> read string
0000000000000108: '# compressed entry header
        type_len    :    5 bits
        time_delta  :   27 bits
        array       :   32 bits

        padding     : type == 29
        time_extend : type == 30
        time_stamp : type == 31
        data max type_len  == 28
'
rfile> help
'read' command:
 To read the current address:
  type '1' or 'x8' for a byte in hex
       'u8' for on unsigned byte
       's8' for a signed byte
  type '2' or 'x16' for short in hex
       'u16' for on unsigned short
       's16' for a signed short
  type '4' or 'x32' for int in hex
       'u32' for on unsigned int
       's32' for a signed int
  type '8' or 'x64' for long long in hex
       'u64' for on unsigned long long
       's64' for a signed long long
  type 'string' followed by optional length
     This will write the string at the location

'dump' command:
 To dump the current location:
  By default, will dump 512 bytes, but if you add
  a length after the command, it will dump that many bytes

'goto' command:
 To goto a location in the file:
  type a value to set the offset into the file.
   Add a '+' to add the current position
   Add a '-' to subtract the current position
rfile> quit
Goodbye!
-----------------------------------------------------------------------------
```

This gives you a command line interface to browse a binary file and walk through
its contents.

## Requirements to build tests:

	* CUnit-devel (CentOS)
	* libcunit1-dev (Ubuntu/Debian)
	* cunit-devel (OpenSUSE)
