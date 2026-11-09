/* Directories whose absolute names are longer than the slot the descriptor
 * table keeps a name in (mcpplibs/openkal-musl#83).
 *
 * A DIRECTORY OPENED BY NAME IS RESOLVED AGAINST AGAIN BY ITS NAME. Before
 * 0.28.1 a name of OKM_DIR_PATH bytes or more was kept truncated, and a
 * relative name opened through the descriptor was joined to the truncated
 * prefix: it named another file, or none. The probe opens chains of 72-byte
 * components through descriptors, crosses the 511/512-byte boundary, works on
 * each level by relative names (open, stat, unlink, rename, rmdir), ascends by
 * `..', changes into the deepest directory, and removes every tree as
 * std::filesystem::remove_all does. A decoy whose name is the truncated prefix
 * of a real directory must stay untouched.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../../tests/mtest_protocol.h"

static int failures;
static void check(int ok, const char* what)
{
	mtp_observe(what, ok);
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) failures++;
}

/* A 72-byte component, distinct per level and per tree. */
static void component(char* out, int tree, int level)
{
	snprintf(out, 73, "t%02d-l%02d-", tree, level);
	size_t n = strlen(out);
	while (n < 72) out[n++] = (char)('a' + (n % 26));
	out[72] = 0;
}

/* Removes everything beneath `dirfd' by relative names only, then itself. */
static int remove_tree(int parent, const char* name)
{
	int fd = openat(parent, name, O_RDONLY | O_DIRECTORY);
	if (fd < 0) return -1;
	DIR* d = fdopendir(fd);
	if (!d) { close(fd); return -1; }
	struct dirent* e;
	int bad = 0;
	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		struct stat st;
		if (fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) { bad = 1; continue; }
		if (S_ISDIR(st.st_mode)) { if (remove_tree(dirfd(d), e->d_name) != 0) bad = 1; }
		else if (unlinkat(dirfd(d), e->d_name, 0) != 0) bad = 1;
	}
	closedir(d);
	if (unlinkat(parent, name, AT_REMOVEDIR) != 0) bad = 1;
	return bad ? -1 : 0;
}

int main(void)
{
	/* The mtest protocol where a runner asked for it (tests/mtest_protocol.h). */
	if (mtp_begin("long-names")) return 0;
	char root[PATH_MAX];
	check(getcwd(root, sizeof root) != 0, "the working directory is known");
	const char* top = "okm-long-names.tmp";
	remove_tree(AT_FDCWD, top);
	check(mkdir(top, 0755) == 0, "a scratch directory is made");
	int topfd = open(top, O_RDONLY | O_DIRECTORY);
	check(topfd >= 0, "and opened");

	enum { TREES = 16, LEVELS = 10 };
	int deep_ok = 1, stat_ok = 1, file_ok = 1, rename_ok = 1, ascend_ok = 1;
	size_t longest = strlen(root) + 1 + strlen(top);
	for (int t = 0; t < TREES; t++) {
		int fd = dup(topfd);
		for (int l = 0; l < LEVELS; l++) {
			char c[73];
			component(c, t, l);
			if (mkdirat(fd, c, 0755) != 0) { deep_ok = 0; break; }
			int next = openat(fd, c, O_RDONLY | O_DIRECTORY);
			if (next < 0) { deep_ok = 0; break; }
			close(fd);
			fd = next;
			if (t == 0) longest += 73;
			/* Relative work at every level, beyond the boundary included. */
			int f = openat(fd, "file.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (f < 0 || write(f, "x", 1) != 1) file_ok = 0;
			if (f >= 0) close(f);
			struct stat st;
			if (fstatat(fd, "file.txt", &st, 0) != 0 || st.st_size != 1) stat_ok = 0;
			if (renameat(fd, "file.txt", fd, "renamed.txt") != 0 || fstatat(fd, "renamed.txt", &st, 0) != 0) rename_ok = 0;
			/* `..' from this level names the previous one, which holds `c'. */
			if (l > 0 && (fstatat(fd, "../renamed.txt", &st, 0) != 0)) ascend_ok = 0;
		}
		if (t == 0) {
			/* Changing into the deepest directory, then working relatively. */
			check(fchdir(fd) == 0, "fchdir into a directory named by more than 512 bytes");
			int f = open("cwd-file.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
			check(f >= 0, "a relative open in that working directory");
			if (f >= 0) close(f);
			struct stat st;
			check(stat("../renamed.txt", &st) == 0, "a relative name that ascends from it");
			char here[PATH_MAX];
			check(getcwd(here, sizeof here) != 0 && strlen(here) == longest,
			      "getcwd reports the whole name");
			check(chdir(root) == 0, "and back");
		}
		close(fd);
	}
	printf("info longest name: %zu bytes\n", longest);
	check(longest > 512, "the deepest names cross the 511/512-byte boundary");
	check(deep_ok, "every level is made and opened through its parent's descriptor");
	check(file_ok, "a file is created by a relative name at every level");
	check(stat_ok, "and found by fstatat");
	check(rename_ok, "and renamed by renameat");
	check(ascend_ok, "`..' from every level names the level above");

	/* A decoy: a real directory whose name is the 511-byte prefix that a
	 * truncated cache would have kept for tree 0's deepest level. A relative
	 * name created through that level's descriptor must not appear in it. */
	char deep[PATH_MAX];
	snprintf(deep, sizeof deep, "%s/%s", root, top);
	for (int l = 0; l < LEVELS; l++) { char c[73]; component(c, 0, l); strcat(deep, "/"); strcat(deep, c); }
	char prefix[512];
	memcpy(prefix, deep, 511); prefix[511] = 0;
	check(strncmp(prefix, root, strlen(root)) == 0 && mkdir(prefix, 0755) == 0,
	      "a decoy named by the truncated prefix is made");
	int deepfd = open(deep, O_RDONLY | O_DIRECTORY);
	check(deepfd >= 0, "the deepest level is opened by its whole name");
	if (deepfd >= 0) {
		int f = openat(deepfd, "after-decoy.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
		check(f >= 0, "a file is created through it");
		if (f >= 0) close(f);
		close(deepfd);
	}
	char inside[PATH_MAX];
	snprintf(inside, sizeof inside, "%s/after-decoy.txt", deep);
	struct stat st;
	check(stat(inside, &st) == 0, "the file is where its directory is");
	snprintf(inside, sizeof inside, "%s/after-decoy.txt", prefix);
	errno = 0;
	check(stat(inside, &st) != 0 && errno == ENOENT, "and not in the decoy");
	check(rmdir(prefix) == 0, "the decoy is still empty");

	/* Removal by relative names only, as remove_all does. */
	int removed = 1;
	for (int t = 0; t < TREES; t++) { char c[73]; component(c, t, 0); if (remove_tree(topfd, c) != 0) removed = 0; }
	check(removed, "every tree is removed through descriptors");
	close(topfd);
	check(remove_tree(AT_FDCWD, top) == 0, "and the scratch directory with what remains");
	errno = 0;
	check(stat(top, &st) != 0 && errno == ENOENT, "nothing is left");
	printf("-- failures: %d --\n", failures);
	return failures ? 1 : 0;
}
