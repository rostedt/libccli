#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <string.h>
#include <libgen.h>
#include <stdlib.h>
#include <stdio.h>
#include <ccli.h>

static int file_completion(struct ccli *ccli, char ***list,
			   int *cnt, int mode, char *match, const char *dirname)
{
	char filename[PATH_MAX];
	struct dirent *dent;
	const char *dname;
	const char *base;
	struct stat st;
	int mode_ifmt = mode & S_IFMT;
	int mode_perm = mode & ~S_IFMT;
	DIR *dir;
	char *m;
	int mlen;
	int len;
	int ret = *cnt;

	mlen = strlen(match);

	m = strdup(match);
	if (!m)
		return -1;

	if (!mlen) {
		base = "";
		dname = "";
	} else if (m[mlen - 1] == '/') {
		base = "";
		dname = m;
	} else {
		for (base = match + strlen(match); base >= match && *base != '/'; base--)
			;
		base++;
		dname = m;
		m[base - match] = '\0';
	}

	len = strlen(base);

	/* dirname comes from the PATH list */
	if (dirname)
		dir = opendir(dirname);
	else
		dir = opendir(dname);
	if (!dir) {
		free(m);
		return -1;
	}

	while ((dent = readdir(dir)) > 0) {
		int type = 0;

		if (strcmp(dent->d_name, ".") == 0 ||
		    strcmp(dent->d_name, "..") == 0) {
			continue;
		}

		if (len && strncmp(dent->d_name, base, len) != 0)
			continue;

		if (dirname)
			snprintf(filename, PATH_MAX, "%s/%s", dirname, dent->d_name);
		else
			snprintf(filename, PATH_MAX, "%s%s", dname, dent->d_name);

		if (stat(filename, &st) == 0) {
			if ((st.st_mode & S_IFMT) == S_IFDIR)
				type = S_IFDIR;
			else if (mode_ifmt && (mode_ifmt != (st.st_mode & S_IFMT)))
				continue;
			else if (mode_perm && !(mode_perm & st.st_mode))
				continue;
		}

		/* If dirname is specified, only list the files */
		if (dirname)
			snprintf(filename, PATH_MAX, "%s", dent->d_name);

		if (type == S_IFDIR) {
			strcat(filename, "/");
			match[mlen] = CCLI_NOSPACE;
		}
		ret = ccli_list_add(ccli, list, cnt, filename);
	}
	closedir(dir);

	free(m);

	return ret;
}

/**
 * ccli_file_completion - helper to find local files
 * @ccli: The ccli descriptor
 * @list: The list of files to return for the completion
 * @cnt: A pointer of the current number of list items.
 * @match: The current match string
 * @PATH: An optional pointer to a PATH environment variable (may be NULL)
 *
 * This is a helper function for the use case of finding files within an
 * option of a command. It will match the @match string (or all files if
 * the string has no length). If @match starts with a '/', then it will do
 * an absolute path lookup. Otherwise it will look at @PATH and parse it like
 * the PATH environment variable, and seach for matches within the directories
 * specified by that variable (using the "path1:path2:path3" syntax).
 *
 * Returns the number of items in list, or -1 on error.
 */
int ccli_file_completion(struct ccli *ccli, char ***list, int *cnt, char *match,
                         int mode, const char *PATH)
{
	char *savetok;
	char *tok;
	char *P;
	char delim = '\0';
	int mlen = strlen(match);
	int ret;

	/* Handle absolute paths */
	if (match[0] == '/')
		return file_completion(ccli, list, cnt, mode, match, NULL);

	/* If PATH is NULL we don't look at anything */
	if (!PATH)
		return 0;

	P = strdup(PATH);
	if (!P)
		return -1;

	for (tok = P, ret = 0; ret >= 0 ; tok = NULL) {
		tok = strtok_r(tok, ":", &savetok);
		if (!tok)
			break;
		ret = file_completion(ccli, list, cnt, mode, match, tok);
		/* Handle modifying of the match end */
		if (delim == '\0')
			delim = match[mlen];
		match[mlen] = '\0';
	}
	free(P);

	/* Update the delim part of match */
	if (delim != '\0')
		match[mlen] = delim;
	return ret;
}
