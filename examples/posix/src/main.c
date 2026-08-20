#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <spawn.h>

static int failures = 0;
static void check(int ok, const char *what) {
	if (!ok) { printf("FAIL: %s (errno=%d)\n", what, errno); failures++; }
	else printf("ok: %s\n", what);
}

static volatile int counter;
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static void *worker(void *p) {
	for (int i = 0; i < 20000; i++) { pthread_mutex_lock(&mtx); counter++; pthread_mutex_unlock(&mtx); }
	return p;
}

int main(int argc, char **argv, char **envp) {
	/* The copy this program starts, so that openkal.process is observed by its
	 * effect rather than by its return value. */
	for (int i = 1; i < argc; i++)
		if (strcmp(argv[i], "--child") == 0) return 41;

	/* Unbuffered, so that a run that stops says how far it got.
	 *
	 * A program whose output is read by another program is fully buffered by
	 * default, which is correct and is exactly wrong here: a probe that stops
	 * before it flushes has reported nothing, and what a reader then has is an
	 * exit status. Every observation below is a line, and a line that was
	 * printed is an observation that was made. */
	setbuf(stdout, NULL);

	printf("-- openkal-musl probe --\n");

	/* stdio, formatting */
	char buf[128];
	int n = snprintf(buf, sizeof buf, "%s/%d/%.3f/%c", "x", 42, 1.5, 'z');
	check(n == 12 && strcmp(buf, "x/42/1.500/z") == 0, "formatted output");

	/* environment */
	check(getenv("PATH") != NULL, "an environment variable is read");
	check(argc >= 1 && argv[0] != NULL, "the argument vector arrived");
	check(envp != NULL && envp == environ, "envp is the environment");

	/* working directory */
	char cwd[4096];
	/* Absolute, in whichever way this system spells absolute. A program that
	 * required a leading separator would be requiring one system's spelling,
	 * and the point of this one is that it was not rewritten for the others. */
	const int rooted = getcwd(cwd, sizeof cwd) != NULL
	                && (cwd[0] == '/' || (cwd[1] == ':' && (cwd[2] == '/' || cwd[2] == '\\')));
	check(rooted, "the working directory is an absolute name");
	printf("   cwd=%s\n", cwd);

	/* files: create, write, seek, read, stat, remove */
	const char *name = "okm-probe.tmp";
	int fd = open(name, O_RDWR | O_CREAT | O_TRUNC, 0644);
	check(fd >= 0, "a file is created");
	check(write(fd, "0123456789", 10) == 10, "ten bytes are written");
	check(lseek(fd, 3, SEEK_SET) == 3, "the position is set");
	char rb[8] = {0};
	check(read(fd, rb, 4) == 4 && memcmp(rb, "3456", 4) == 0, "four bytes are read from the position");
	struct stat st;
	check(fstat(fd, &st) == 0 && st.st_size == 10, "the open file reports its length");
	check(ftruncate(fd, 4) == 0, "the length is set");
	check(fstat(fd, &st) == 0 && st.st_size == 4, "the new length is reported");
	close(fd);
	check(stat(name, &st) == 0 && S_ISREG(st.st_mode), "the name refers to a regular file");

	/* stdio over the same file */
	FILE *f = fopen(name, "w");
	check(f != NULL, "the file opens through stdio");
	fprintf(f, "line one\nline two\n");
	fclose(f);
	f = fopen(name, "r");
	char line[64];
	check(fgets(line, sizeof line, f) && strcmp(line, "line one\n") == 0, "a line is read back");
	fclose(f);

	/* append */
	f = fopen(name, "a");
	fprintf(f, "line three\n");
	fclose(f);
	f = fopen(name, "r");
	int lines = 0;
	while (fgets(line, sizeof line, f)) lines++;
	fclose(f);
	check(lines == 3, "appending added a line without replacing the file");

	/* directories */
	check(mkdir("okm-probe.dir", 0755) == 0, "a directory is created");
	f = fopen("okm-probe.dir/a", "w"); if (f) fclose(f);
	f = fopen("okm-probe.dir/b", "w"); if (f) fclose(f);
	DIR *d = opendir("okm-probe.dir");
	check(d != NULL, "a directory is opened");
	int entries = 0;
	struct dirent *de;
	while (d && (de = readdir(d))) if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) entries++;
	if (d) closedir(d);
	check(entries == 2, "the directory reports both entries");
	unlink("okm-probe.dir/a"); unlink("okm-probe.dir/b");
	check(rmdir("okm-probe.dir") == 0, "the directory is removed");

	/* rename and absence */
	check(rename(name, "okm-probe2.tmp") == 0, "a file is renamed");
	check(access(name, F_OK) != 0 && errno == ENOENT, "the old name is gone, and says so");
	check(unlink("okm-probe2.tmp") == 0, "the new name is removed");

	/* time */
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	struct timespec req = { 0, 20 * 1000 * 1000 };
	nanosleep(&req, NULL);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	long long el = (long long)(t1.tv_sec - t0.tv_sec) * 1000000000 + (t1.tv_nsec - t0.tv_nsec);
	check(el >= 20000000, "the monotonic clock advanced by at least the requested interval");
	check(time(NULL) > 1700000000, "the wall clock reports a plausible time");

	/* threads and mutexes: contended */
	pthread_t th[4];
	counter = 0;
	int started = 0;
	for (int i = 0; i < 4; i++) if (pthread_create(&th[i], NULL, worker, NULL) == 0) started++;
	for (int i = 0; i < started; i++) pthread_join(th[i], NULL);
	check(started == 4, "four execution contexts started");
	check(counter == 80000, "80000 increments, none lost");
	printf("   counter=%d\n", counter);

	/* memory */
	size_t big = 1 << 20;
	char *p = malloc(big);
	check(p != NULL, "a large allocation succeeds");
	memset(p, 0xab, big);
	check((unsigned char)p[big - 1] == 0xab, "the whole allocation is writable");
	p = realloc(p, big * 2);
	check(p != NULL && (unsigned char)p[big - 1] == 0xab, "reallocation preserves the contents");
	free(p);

	/* Starting another program. The program started is this one, with an
	 * argument that tells it to report a status and stop: a program that named
	 * one of the system's own would be naming a system, and every system has a
	 * different set. */
	pid_t pid;
	char *sargv[] = { argv[0], "--child", NULL };
	extern char **environ;
	const int rc = posix_spawn(&pid, argv[0], NULL, NULL, sargv, environ);
	if (rc == 0) {
		int status = 0;
		check(waitpid(pid, &status, 0) == pid, "the started program is awaited");
		check(WIFEXITED(status) && WEXITSTATUS(status) == 41,
		      "it reported the status it was written to report");
	} else {
		printf("FAIL: a program is started (posix_spawn returned %d)\n", rc);
		failures += 2;
	}

	printf("-- failures: %d --\n", failures);
	return failures ? 1 : 0;
}
