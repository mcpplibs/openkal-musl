/* Another program, started three ways, by a program that names no openkal
 * symbol.
 *
 *   fork      duplicating the calling image --- `openkal.space'
 *   system    a command line handed to a shell --- `openkal.process'
 *   popen     the same, with a channel back --- `openkal.process' twice over
 *
 * ⭐ TWO OF THE THREE ALREADY WORKED AND NOTHING SAID SO. This package replaces
 * musl's `posix_spawn' with port/src/okm_spawn.c, and `system' and `popen' are
 * both written on `posix_spawn' --- so they have been available for as long as
 * that file has, and no test in this repository ever ran one. A capability that
 * works and is not asserted is a capability that will stop working quietly;
 * these three lines are what keeps that from happening again.
 *
 * ⚠️ WHAT IS EXPECTED IS STATED ON THE COMMAND LINE RATHER THAN INFERRED, which
 * is the same arrangement examples/net uses and for the same reason. An
 * environment whose backend declines `openkal.space' is not a failure; an
 * environment that was expected to provide it and quietly does not IS one, and
 * a probe that accepted either answer could not tell them apart.
 *
 *   --fork | --no-fork     whether the calling image can be duplicated
 *   --shell | --no-shell   whether this system has a shell at /bin/sh
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures = 0;

static void check(int ok, const char* what)
{
	if (!ok) { printf("FAIL: %s (errno=%d)\n", what, errno); failures++; }
	else printf("ok: %s\n", what);
}

int main(int argc, char** argv)
{
	setbuf(stdout, NULL);

	int expect_fork = -1, expect_shell = -1;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--fork") == 0) expect_fork = 1;
		else if (strcmp(argv[i], "--no-fork") == 0) expect_fork = 0;
		else if (strcmp(argv[i], "--shell") == 0) expect_shell = 1;
		else if (strcmp(argv[i], "--no-shell") == 0) expect_shell = 0;
	}
	if (expect_fork < 0 || expect_shell < 0) {
		printf("usage: subprocess --fork|--no-fork --shell|--no-shell\n");
		return 2;
	}

	printf("-- openkal-musl subprocess probe --\n");

	/* --- duplicating the calling image ---------------------------------------- */

	errno = 0;
	const pid_t kid = fork();
	if (kid == 0) {
		/* ⚠️ `_exit' AND NOT `exit'. The copy holds a copy of the parent's stdio
		 * buffers, and running the exit handlers would write them a second
		 * time --- which is a defect of the probe and looks like one of the
		 * port. */
		_exit(23);
	}
	if (expect_fork) {
		check(kid > 0, "the calling image is duplicated");
		if (kid > 0) {
			int status = 0;
			check(waitpid(kid, &status, 0) == kid, "the copy is awaited");
			check(WIFEXITED(status) && WEXITSTATUS(status) == 23,
			      "and it reported the status it was written to report");
		} else {
			failures += 2;
		}
	} else {
		/* ⭐ A REFUSAL IS THE EXPECTED ANSWER HERE AND IS CHECKED AS ONE. Clause
		 * 3 permits an implementation to decline an interface in whole, clause
		 * 6.1 makes the absence a link-time one, and okm_opt.h's rule turns it
		 * into the defined error a POSIX caller already handles. */
		check(kid < 0 && errno == ENOSYS,
		      "duplicating the calling image is refused, as this system requires");
	}

	if (!expect_shell) {
		printf("   this system has no shell at /bin/sh; system and popen are not asked\n");
		printf("-- failures: %d --\n", failures);
		return failures ? 1 : 0;
	}

	/* --- a command line handed to a shell -------------------------------------- */

	const int rc = system("exit 5");
	/* The status is in the shape `wait' reports: the low seven bits name a
	 * signal and are zero when the program ended by returning. */
	check(rc >= 0 && WIFEXITED(rc) && WEXITSTATUS(rc) == 5,
	      "a command line runs and its status comes back");

	/* --- and the same, with a channel back ------------------------------------- */

	FILE* f = popen("echo carried-back", "r");
	check(f != NULL, "a command line runs with a channel back");
	if (f) {
		char line[128];
		memset(line, 0, sizeof line);
		const int got = fgets(line, sizeof line, f) != NULL;
		check(got && strncmp(line, "carried-back", 12) == 0,
		      "and what it wrote arrives through the channel");
		check(pclose(f) == 0, "the channel closes with the command's own status");
	} else {
		failures += 2;
	}

	printf("-- failures: %d --\n", failures);
	return failures ? 1 : 0;
}
