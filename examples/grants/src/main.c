/* A program given fewer directories than the whole file system gives them on
 * (openkal-musl 0.27.0, port/src/okm_spawn.c; openkal 0.23).
 *
 * This program starts itself through openkal with two directories granted,
 * both read-only: a directory of its own, as the one it runs in, and the
 * directory its file is in. That program starts itself again through
 * `posix_spawn', which names no directories; the third program tries to create
 * a file in the directory it runs in. Given on, the directories are read-only
 * for it too, and the creation is refused; given the implementation's
 * defaults, it would be written. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <openkal.h>
#include <okm_preopen.h>

extern char** environ;

static int failures;
static void check(int held, const char* what)
{
	printf("%s: %s\n", held ? "ok" : "FAIL", what);
	if (!held) failures++;
}

static char g_self[4096];

/* The third program: the directory it runs in, written. */
static int grandchild(void)
{
	char where[4096];
	if (!getcwd(where, sizeof where)) return 70;
	const int fd = open("created-by-the-third.txt", O_CREAT | O_WRONLY, 0600);
	if (fd >= 0) { close(fd); unlink("created-by-the-third.txt"); return 61; }
	return 60;
}

/* A program that records what it reaches of the root (okm_preopen.h): an
 * absent name asked about, and a present file read. */
static int recording(const char* dir)
{
	char record[4096], name[4096];
	snprintf(record, sizeof record, "%s/record", dir);
	if (okm_preopen_attenuate("/", KAL_DIR_RECORDED, record) != 0) return 80;
	snprintf(name, sizeof name, "%s/absent.h", dir);
	if (access(name, F_OK) == 0) return 81;
	snprintf(name, sizeof name, "%s/present.txt", dir);
	const int fd = open(name, O_RDONLY);
	if (fd < 0) return 82;
	close(fd);
	return 0;
}

/* Whether the record holds the entry `kind' and `dir/leaf', the name relative
 * to the root. */
static int recorded(const char* dir, char kind, const char* leaf)
{
	char record[4096], want[4096], buf[65536];
	snprintf(record, sizeof record, "%s/record", dir);
	const int n = snprintf(want, sizeof want, "%c%s/%s", kind, dir + 1, leaf) + 1;
	const int fd = open(record, O_RDONLY);
	if (fd < 0) return 0;
	const ssize_t got = read(fd, buf, sizeof buf);
	close(fd);
	for (ssize_t at = 0; at < got; at += (ssize_t)strlen(buf + at) + 1)
		if ((ssize_t)n - 1 == (ssize_t)strlen(buf + at) && !memcmp(buf + at, want, n)) return 1;
	return 0;
}

/* The second program: the third, through posix_spawn. */
static int child(void)
{
	char* argv[] = { g_self, "--third", 0 };
	pid_t pid;
	const int e = posix_spawn(&pid, g_self, 0, 0, argv, environ);
	if (e) return 50;
	int status = 0;
	if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) return 51;
	return WEXITSTATUS(status);
}

static struct kal_dir root(void)
{
	for (kal_uintptr i = 0; i < kal_fs_preopen_count(); ++i) {
		struct kal_dir d = { 0 }; char n[8]; kal_uintptr l = 0;
		if (kal_fs_preopen(i, &d, n, sizeof n, &l) == kal_ok && l == 1 && n[0] == '/') return d;
	}
	struct kal_dir none = { 0 };
	return none;
}

static int read_only(struct kal_dir from, const char* path, struct kal_dir* out)
{
	struct kal_dir d = { 0 };
	int e = kal_fs_open_dir(from, path + 1, strlen(path + 1), &d);
	if (e != kal_ok) { printf("   %s is not opened: %d\n", path, e); return 0; }
	struct kal_dir_rights r = { 0 };
	r.self_size = sizeof r;
	r.flags = KAL_DIR_READ_ONLY;
	e = kal_fs_dir_attenuate(d, &r, out);
	if (e != kal_ok) printf("   %s is not attenuated: %d\n", path, e);
	return e == kal_ok;
}

int main(int argc, char** argv)
{
	const kal_intptr n = kal_env_image(g_self, sizeof g_self - 1);
	if (n <= 0) { printf("FAIL: the program's own file is named\n"); return 1; }
	g_self[n] = 0;
	/* The program's name as it was started may be relative to the directory
	 * it was started in; the starts below name it from the root. */
	char named[4096];
	if (realpath(g_self, named)) strcpy(g_self, named);
	if (argc > 1 && !strcmp(argv[1], "--second")) return child();
	if (argc > 1 && !strcmp(argv[1], "--third")) return grandchild();
	if (argc > 2 && !strcmp(argv[1], "--recording")) return recording(argv[2]);

	printf("-- openkal-musl grants probe --\n");
	if (!(kal_process_props() & KAL_PROCESS_PROP_GRANT_DIR)
	    || !(kal_fs_props(root()) & KAL_FS_PROP_ATTENUATE)) {
		printf("this system grants no directories, or does not attenuate them\n-- failures: 0 --\n");
		return 0;
	}
	char made[] = "/tmp/okm-grants-XXXXXX";
	check(mkdtemp(made) != 0, "a directory of the program's own");
	/* By the names the environment resolves them to: a grant is opened
	 * beneath the root, and on macOS /tmp is a symbolic link elsewhere
	 * (/private/tmp), which a name opened beneath a directory does not leave
	 * it to follow. */
	char dir[4096], bin[4096];
	check(realpath(made, dir) != 0, "the directory's name, resolved");
	strcpy(bin, g_self);
	*strrchr(bin, '/') = 0;
	struct kal_dir mine = { 0 }, its = { 0 };
	check(read_only(root(), dir, &mine) && read_only(root(), bin, &its), "both directories, read-only");
	const struct kal_preopen grants[2] = { { mine, dir, strlen(dir) }, { its, bin, strlen(bin) } };
	const char* args[] = { g_self, "--second" };
	const kal_uintptr lens[] = { strlen(g_self), 8 };
	const struct kal_spawn how = { root(), mine, 0, grants, 2, 0 };
	struct kal_process p = { 0 };
	int status = -1, terminated = 0;
	const int started = kal_process_spawn(&how, g_self + 1, strlen(g_self + 1), args, lens, 2, 0, 0, 0, 0, &p);
	check(started == kal_ok, "the second program starts with the two");
	if (started != kal_ok) printf("   %s is not started: %d\n", g_self, started);
	if (started == kal_ok) { kal_process_wait(p, &status, &terminated); kal_process_close(p); }
	printf("   the third program reported %d\n", status);
	check(!terminated && status != 61, "a program it starts holds them read-only too: it creates nothing there");
	check(!terminated && status == 60, "and is refused the creation");

	/* A HELD DIRECTORY RECORDED AT THE PROGRAM'S OWN REQUEST: a program started
	 * through posix_spawn records the root, asks about an absent name and reads
	 * a file; the record names both, relative to the root, and ends with the
	 * program's end. */
	{
		char present[4096];
		snprintf(present, sizeof present, "%s/present.txt", dir);
		const int fd = open(present, O_CREAT | O_WRONLY, 0600);
		if (fd >= 0) close(fd);
		char* rargv[] = { g_self, "--recording", dir, 0 };
		pid_t pid;
		int status = -1;
		const int e = posix_spawn(&pid, g_self, 0, 0, rargv, environ);
		if (!e) waitpid(pid, &status, 0);
		check(!e && WIFEXITED(status) && WEXITSTATUS(status) == 0, "a program records the root it holds");
		if (e || !WIFEXITED(status) || WEXITSTATUS(status)) printf("   it reported %d\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
		check(recorded(dir, 'a', "absent.h"), "the record names the absent name it asked about");
		check(recorded(dir, 'r', "present.txt"), "and the file it read");
		char record[4096];
		snprintf(record, sizeof record, "%s/record", dir);
		unlink(record); unlink(present);
	}
	rmdir(made);
	printf("-- failures: %d --\n", failures);
	return failures != 0;
}
