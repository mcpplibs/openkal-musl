#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/utsname.h>
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

	/* THE LENGTH IS ALSO SET BY NAME, AND THAT IS NOT THE SAME OPERATION.
	 *
	 * `ftruncate' above is observed and `truncate' was not, and only the first
	 * had a case in the dispatcher: the second reported ENOSYS. The two are not
	 * interchangeable to a caller holding a name and no open file, which is
	 * every caller of `std::filesystem::resize_file' --- libc++ expresses it as
	 * `truncate(p.c_str(), size)'. The descriptor is closed above, so nothing
	 * here could reach the operation that already worked. */
	check(truncate(name, 2) == 0, "the length is set by name");
	check(stat(name, &st) == 0 && st.st_size == 2,
	      "the length set by name is the length reported");

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

	/* READING A DIRECTORY'S MODIFICATION TIME WORKS, AND IT IS OBSERVED HERE
	 * SEPARATELY FROM SETTING IT BECAUSE THE C++ LIBRARY ABOVE GIVES THEM ONE
	 * NAME.
	 *
	 * libc++ throws `filesystem error: in last_write_time' for both overloads,
	 * so a caller that reads a lock directory's timestamp to decide staleness
	 * and writes it to refresh the lock cannot tell from the message which of
	 * the two failed. It was reported as the first in openkal-linux#13 and it
	 * was the second: `stat' resolves a directory perfectly well.
	 *
	 * ⭐ SETTING IT USED TO BE ASSERTED HERE AS A REFUSAL, and this comment said
	 * that if openkal gained the operation this observation would be the one to
	 * say the row was out of date. It did its job: 0.10.0 opens a directory for
	 * READING to stamp it --- which Linux and macOS perform and Windows does not
	 * --- so the answer now depends on the implementation beneath, and an
	 * observation that depends on the backend belongs where the backend is
	 * stated on the command line. It moved to `examples/surface', under
	 * `--dir-time | --no-dir-time'. Only the reading half is left here. */
	struct stat ds;
	check(stat("okm-probe.dir", &ds) == 0 && S_ISDIR(ds.st_mode) && ds.st_mtime > 0,
	      "a directory reports its modification time");

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

	/* ⭐⭐ THE DISPOSITION OF A SIGNAL IS TOUCHED, WHICH NOTHING HERE DID.
	 *
	 * This file had thirty-six observations and three of them were about
	 * `abort'. It contained no call to `signal' or `sigaction' anywhere --- so
	 * it examined whether `abort' ENDS the program and never whether a program
	 * may ASK what a signal is set to. A defect that killed any program doing
	 * the second passed every one of the thirty-six.
	 *
	 * ⚠️ ALL THREE FORMS, AND SIGABRT AMONG THEM. The C library takes a lock for
	 * any change to that one disposition and blocks signals to take it, so
	 * SIGABRT reaches code the others do not --- and the enquiry, which changes
	 * nothing, reached it too. Two of the three forms below would have passed
	 * while the third killed the process. */
	{
		int survived = 1;
		for (int sig = 1; sig < 32; sig++) {
			if (sig == SIGKILL || sig == SIGSTOP) continue;
			struct sigaction seen;
			memset(&seen, 0, sizeof seen);
			/* An enquiry, which changes nothing. */
			const int q = sigaction(sig, NULL, &seen);
			if (q != 0 && errno != ENOSYS) survived = 0;
			/* Ignoring, which every environment can express. */
			errno = 0;
			signal(sig, SIG_IGN);
			if (errno != 0 && errno != ENOSYS) survived = 0;
			/* A handler, which this environment cannot deliver and refuses. */
			errno = 0;
			if (signal(sig, SIG_DFL) == SIG_ERR && errno != ENOSYS) survived = 0;
		}
		check(survived, "every signal's disposition may be read and written or refused");
	}

	/* Nodes whose content is another name, where the volume has them. */
	{
		/* The names this block uses are its own: the file the earlier
		 * observations made has been removed by the time this runs, and a probe
		 * that depended on another probe's leftovers would report an absence as
		 * a defect. */
		{ FILE *t = fopen("okm-link-target.tmp", "w"); if (t) { fputs("0123456789", t); fclose(t); } }
		unlink("okm-probe-link");
		const int made = symlink("okm-link-target.tmp", "okm-probe-link");
		if (made == 0) {
			char target[64] = { 0 };
			const ssize_t got = readlink("okm-probe-link", target, sizeof target - 1);
			check(got == (ssize_t)strlen("okm-link-target.tmp")
			          && strcmp(target, "okm-link-target.tmp") == 0,
			      "a node's content reads back as it was written");

			/* ⭐ THE OBSERVATION THE PORT MOST NEEDED. Asking resolves and
			 * opening resolves, so the two agree; asking with the flag reports
			 * the node itself. They disagreed, and a C++ library above reported
			 * a link where a caller would have reached a file. */
			struct stat followed, itself;
			check(stat("okm-probe-link", &followed) == 0 && S_ISREG(followed.st_mode),
			      "stat resolves, and reports what the name finally refers to");
			check(lstat("okm-probe-link", &itself) == 0 && S_ISLNK(itself.st_mode),
			      "lstat reports the node itself");

			/* ⭐⭐ AND THE THIRD QUESTION, WHICH IS NEITHER OF THOSE TWO.
			 *
			 * O_NOFOLLOW does not ask to open the link and does not ask to
			 * open its target: it asks `is this name a link?' and expects
			 * ELOOP when it is. openkal offers no opening that declines to
			 * resolve --- by design --- so this port resolved, and for a link
			 * to a name that is absent it answered ENOENT.
			 *
			 * ⚠️ THAT IS A DIFFERENT ANSWER TO A DIFFERENT QUESTION, AND
			 * NOTHING NEARBY LOOKED WRONG. Every operation above still held.
			 * What failed was three layers up: libc++'s `remove_all' descends
			 * by opening each entry O_DIRECTORY|O_NOFOLLOW and reads ENOENT as
			 * `it is already gone', so it unlinked nothing and then reported
			 * ENOTEMPTY for a directory it had just declined to empty. The
			 * host toolchain removed the same tree.
			 *
			 * The answer comes from the enquiry openkal 0.9 added: ask about
			 * the name itself. Both cases are checked because they fail
			 * differently --- a live target resolved to a FILE and returned a
			 * descriptor, which is not an error at all. */
			int nf = open("okm-probe-link", O_RDONLY | O_NOFOLLOW);
			check(nf < 0 && errno == ELOOP,
			      "opening a link with O_NOFOLLOW reports that it is a link");
			if (nf >= 0) close(nf);

			unlink("okm-probe-target-gone");
			unlink("okm-probe-dangling");
			if (symlink("okm-probe-target-gone", "okm-probe-dangling") == 0) {
				nf = open("okm-probe-dangling", O_RDONLY | O_NOFOLLOW);
				check(nf < 0 && errno == ELOOP,
				      "and does so for a link whose target is absent, rather than ENOENT");
				if (nf >= 0) close(nf);
				check(unlink("okm-probe-dangling") == 0,
				      "a link whose target is absent is still removable");
			}

			unlink("okm-probe-link");
		} else if (errno == ENOSYS || errno == EPERM) {
			printf("ok:   this volume has no nodes that name others, which it reported\n");
		} else {
			printf("FAIL: making a node that names another (errno %d)\n", errno);
			failures++;
		}
		unlink("okm-link-target.tmp");
	}

	/* ⚠️ Two different files are two different files. `st_dev' and `st_ino'
	 * were constants, so every file compared equal to every other and a C++
	 * library's `equivalent' answered true with no error.
	 *
	 * ⚠️⚠️ WHEN THIS FAILS, THE DEFECT IS USUALLY NOT IN THIS PACKAGE. This
	 * port copies the identity out of `kal_node_info' and puts zero there when
	 * the implementation does not report one --- which is permitted, and which
	 * makes every node compare equal to every other. So a failure here says
	 * "the openkal implementation beneath this one declined to report an
	 * identity", and the place to look is its `kal_fs_info'.
	 *
	 * Measured: it failed on Windows, and openkal-windows was reading a volume
	 * serial number the object manager had written and then discarding it,
	 * because the enquiry reported STATUS_BUFFER_OVERFLOW for a volume label
	 * that did not fit and the implementation read that as a failure. The
	 * conformance suite could not have said so: an implementation is allowed
	 * to decline the field, so the suite reports the observation as one it did
	 * not make. This is the criterion that notices, and it is two packages
	 * away from the defect. */
	{
		struct stat x, y;
		FILE *fx = fopen("okm-probe-x.tmp", "w"); if (fx) fclose(fx);
		FILE *fy = fopen("okm-probe-y.tmp", "w"); if (fy) fclose(fy);
		check(stat("okm-probe-x.tmp", &x) == 0 && stat("okm-probe-y.tmp", &y) == 0
		          && !(x.st_dev == y.st_dev && x.st_ino == y.st_ino),
		      "two different files have different identities");
		unlink("okm-probe-x.tmp");
		unlink("okm-probe-y.tmp");
	}

	/* A value POSIX says cannot fail is not a negated error. */
	check(getpgrp() > 0, "the process group is a number and not a negated error");

	/* The page is the machine's and not the build's.
	 *
	 * ⚠️⚠️ AND "POSITIVE POWER OF TWO" WAS TRUE OF THE VALUE THAT BROKE IT.
	 * This library took `kal_memory_granularity()' as its page size, and an
	 * implementation for a machine with no memory management unit answers ONE
	 * --- correctly, since nothing there needs rounding. One is positive and
	 * one is a power of two, so this assertion held while the allocator asked
	 * the environment for one-byte extents and the program stopped inside the
	 * first allocation that needed a new one.
	 *
	 * ⭐ SO THE CRITERION IS WHAT THE ALLOCATOR REQUIRES, NOT WHAT THE NUMBER
	 * LOOKS LIKE. A page smaller than this library's own quantum is not a page
	 * this library can use, whatever openkal reports. */
	{
		const long page = sysconf(_SC_PAGESIZE);
		check(page >= 4096 && (page & (page - 1)) == 0,
		      "the page size is a power of two no smaller than the allocator's quantum");

		/* And the property the number exists to have. Several pages, written
		 * end to end: the allocation this library rounds to `page' and the
		 * memory it hands back are the same memory. */
		const size_t span = (size_t)page * 4 + 17;
		unsigned char *big = malloc(span);
		int whole = big != NULL;
		if (big) {
			for (size_t i = 0; i < span; i++) big[i] = (unsigned char)(i * 31u);
			for (size_t i = 0; i < span; i++)
				if (big[i] != (unsigned char)(i * 31u)) { whole = 0; break; }
			free(big);
		}
		check(whole, "several pages are obtained in one allocation and every byte of it holds");
	}

	/* WHAT VERSION OF THE C LIBRARY THIS PROGRAM HOLDS.
	 *
	 * The release field was the string literal "0.5.0" through every release
	 * after 0.5.0, so a program that asked was given a false answer rather than
	 * no answer. It now comes from the manifest. This asserts that it is neither
	 * absent nor the placeholder a build that could not read the manifest would
	 * leave -- it deliberately does NOT assert a particular number, because the
	 * number moves at every release and an observation naming one would have to
	 * be edited by every release rather than checked by it.
	 *
	 * The build that DOES check the number is the workflow, which compares this
	 * line against mcpp.toml. Reported in openkal-linux#13, where two rounds were
	 * spent establishing which version a consumer had actually built. */
	{
		struct utsname un;
		const int ok = uname(&un) == 0;
		check(ok, "the system reports its identity");
		if (ok) {
			printf("note: release=%s\n", un.release);
			check(un.release[0] != '\0' && strcmp(un.release, "unknown") != 0,
			      "the C library reports a version rather than a placeholder");
		}
	}

	printf("-- failures: %d --\n", failures);
	return failures ? 1 : 0;
}
