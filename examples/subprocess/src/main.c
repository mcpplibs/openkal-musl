/* Another program, started several ways, by a program that names no openkal
 * symbol.
 *
 *   fork          duplicating the calling image --- `openkal.space'
 *   posix_spawn   starting a named program --- `openkal.process'
 *   execve        the same, expressed as replacing this image
 *   system        a command line handed to a shell
 *   popen         the same, with a channel back
 *
 * ⭐ TWO OF THE FIRST THREE ALREADY WORKED AND NOTHING SAID SO. This package
 * replaces musl's `posix_spawn' with port/src/okm_spawn.c, and `system' and
 * `popen' are both written on `posix_spawn' --- so they have been available for
 * as long as that file has, and no test in this repository ever ran one. A
 * capability that works and is not asserted is a capability that will stop
 * working quietly.
 *
 * ⚠️⚠️ AND THAT IS EXACTLY HOW THE REDIRECTION DEFECT SURVIVED. Every
 * observation here reported that a program STARTED. None of them asked WHERE ITS
 * OUTPUT WENT, and the answer was: to the stream this program was started with,
 * whatever the caller had redirected onto. `popen' passed throughout, because
 * musl builds it on an explicit `adddup2' and that branch was translated;
 * `system', `posix_spawn' and `execve' all lost the redirection. Reported by a
 * consumer as openkal-linux#13, not here, and the whole reason it was not here
 * is the shape of the question this file used to ask.
 *
 * ⭐ THE PROGRAM THAT IS STARTED IS THIS ONE, WITH A MODE ON ITS COMMAND LINE.
 * A probe that started `/bin/sh' would be a probe that only two of the four
 * systems can run, and the redirection criteria hold on all four.
 *
 * ⚠️ WHAT IS EXPECTED IS STATED ON THE COMMAND LINE RATHER THAN INFERRED, which
 * is the same arrangement examples/net uses and for the same reason. An
 * environment whose backend declines `openkal.space' is not a failure; an
 * environment that was expected to provide it and quietly does not IS one, and
 * a probe that accepted either answer could not tell them apart.
 *
 *   --fork | --no-fork     whether the calling image can be duplicated
 *   --shell | --no-shell   whether this system has a shell at /bin/sh
 *
 *   --abort-signal | --abort-status | --abort-terminated
 *                          how the implementation beneath expresses a program
 *                          that ended abnormally.
 *
 * ⚠️⚠️ THREE VALUES AND NOT TWO, BECAUSE A CRITERION WHOSE ANSWER IS "SOMETHING
 * UNUSUAL HAPPENED" HOLDS FOR THE DEFECT AS WELL AS FOR THE FIX.
 *
 * Before `abort' reached `kal_abort' it fell through to musl's `a_crash()' and
 * the program died on a fault --- which is also distinguishable from an
 * ordinary end, so the portable observation below passes either way and proves
 * nothing on its own. What discriminates is the PARTICULAR end each
 * implementation produces:
 *
 *     --abort-signal      SIGABRT.  openkal-linux's kal_abort is tgkill(SIGABRT)
 *     --abort-status      exit 134. openkal-macos ends with a status no program
 *                                   returns
 *     --abort-terminated  a signal, any signal, and not a fault. On Windows
 *                                   kal_abort fail-fasts and kal_process_wait
 *                                   reports that as a termination; the fault
 *                                   this replaces was reported as an exit
 *
 * So each row asserts what ITS backend does, and each of the three fails on the
 * defect. openkal permits implementations to differ here and the criterion has
 * to differ with them rather than settle for what all three happen to share.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

/* ⚠️ THE FOUR REDIRECTED MARKERS SHARE A SUFFIX AND THE INHERITED ONE DOES NOT.
 *
 * The criterion this probe cannot state by itself is that the started program's
 * bytes did NOT arrive on the stream this program was started with --- a program
 * cannot read its own output. Continuous integration states it, over the log
 * this probe's output is captured into, in both directions: nothing matching
 * `redirected-bytes' may appear there, and `inherited-child-bytes' must.
 *
 * So no message below may contain either token. A failure reports the LENGTH it
 * found rather than the content, for the same reason. */
#define MARK_SPAWN   "spawn-redirected-bytes"
#define MARK_EXECVE  "execve-redirected-bytes"
#define MARK_SHELL   "shell-redirected-bytes"
#define MARK_ADDOPEN "addopen-redirected-bytes"
#define MARK_INHERIT "inherited-child-bytes"

static int failures = 0;

static void check(int ok, const char* what)
{
	if (!ok) { printf("FAIL: %s (errno=%d)\n", what, errno); failures++; }
	else printf("ok: %s\n", what);
}

/* --- the modes this program is started in --------------------------------- */

static int child_mode(int argc, char** argv)
{
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--child-echo") == 0 && i + 1 < argc) {
			/* ⚠️ `write' AND NOT `printf'. What is being measured is which
			 * stream descriptor 1 names in this program, and stdio would add a
			 * buffer between the question and the answer. */
			const char* t = argv[i + 1];
			(void)!write(STDOUT_FILENO, t, strlen(t));
			return 0;
		}
		if (strcmp(argv[i], "--child-abort") == 0) {
			abort();
		}
		if (strcmp(argv[i], "--child-exit") == 0 && i + 1 < argc) {
			_exit(atoi(argv[i + 1]));
		}
		if (strcmp(argv[i], "--child-sleep") == 0 && i + 1 < argc) {
			usleep((unsigned)atoi(argv[i + 1]) * 1000u);
			_exit(7);
		}
	}
	return -1;
}

/* --- reading back what a started program wrote ----------------------------- */

/* The whole of a file, or -1. The buffer is the caller's and the count is the
 * answer: a comparison against a marker must be able to tell "the marker" from
 * "the marker twice", which a string comparison alone cannot. */
static long slurp(const char* path, char* buf, size_t cap)
{
	const int fd = open(path, O_RDONLY);
	if (fd < 0) return -1;
	long total = 0;
	for (;;) {
		if ((size_t)total >= cap) break;
		const ssize_t n = read(fd, buf + total, cap - (size_t)total);
		if (n <= 0) break;
		total += (long)n;
	}
	close(fd);
	buf[total] = 0;
	return total;
}

/* Whether a file holds exactly the marker, allowing the line ending a shell's
 * `echo' appends. The trailing bytes are removed rather than ignored, so that a
 * file holding the marker TWICE still fails. */
static int holds(const char* path, const char* marker, long* got)
{
	char buf[512];
	long n = slurp(path, buf, sizeof buf - 1);
	if (got) *got = n;
	if (n < 0) return 0;
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
	return (size_t)n == strlen(marker) && memcmp(buf, marker, (size_t)n) == 0;
}

/* --- a redirection this program performed, and the program it starts -------- */

/* ⚠️ NOTHING MAY BE PRINTED WHILE DESCRIPTOR 1 IS THE SINK, because this
 * program's own report would land in the file it is about to read back. Each
 * experiment therefore returns its findings and reports them afterwards. */
static int redirect_stdout(const char* path, int* saved)
{
	fflush(stdout);
	*saved = dup(STDOUT_FILENO);
	if (*saved < 0) return -1;
	const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0) return -1;
	const int r = dup2(fd, STDOUT_FILENO);
	close(fd);
	return r < 0 ? -1 : 0;
}

static void restore_stdout(int saved)
{
	if (saved < 0) return;
	fflush(stdout);
	dup2(saved, STDOUT_FILENO);
	close(saved);
}

static void wait_for(pid_t pid)
{
	int status = 0;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
}

int main(int argc, char** argv)
{
	setbuf(stdout, NULL);

	if (child_mode(argc, argv) == 0) return 0;

	enum { ABORT_UNSET = 0, ABORT_SIGABRT, ABORT_STATUS_134, ABORT_TERMINATED };
	int expect_fork = -1, expect_shell = -1, expect_abort = ABORT_UNSET;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--fork") == 0) expect_fork = 1;
		else if (strcmp(argv[i], "--no-fork") == 0) expect_fork = 0;
		else if (strcmp(argv[i], "--shell") == 0) expect_shell = 1;
		else if (strcmp(argv[i], "--no-shell") == 0) expect_shell = 0;
		else if (strcmp(argv[i], "--abort-signal") == 0) expect_abort = ABORT_SIGABRT;
		else if (strcmp(argv[i], "--abort-status") == 0) expect_abort = ABORT_STATUS_134;
		else if (strcmp(argv[i], "--abort-terminated") == 0) expect_abort = ABORT_TERMINATED;
	}
	if (expect_fork < 0 || expect_shell < 0 || expect_abort == ABORT_UNSET) {
		printf("usage: subprocess --fork|--no-fork --shell|--no-shell"
		       " --abort-signal|--abort-status|--abort-terminated\n");
		return 2;
	}

	const char* self = argv[0];

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
			const int ok = WIFEXITED(status) && WEXITSTATUS(status) == 23;
			/* ⚠️ THE RAW STATUS IS PRINTED WHEN IT IS WRONG, and only then. A
			 * line reading "it did not report the status it was written to
			 * report" names a fault and not a place: a copy that ended on a
			 * signal and one that returned the wrong number are different
			 * failures, and the number is what tells them apart. */
			if (!ok)
				printf("   status=0x%x exited=%d code=%d signalled=%d signal=%d\n",
				       (unsigned)status, WIFEXITED(status), WEXITSTATUS(status),
				       WIFSIGNALED(status), WTERMSIG(status));
			check(ok, "and it reported the status it was written to report");
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

	/* --- the control: a program started with nothing redirected --------------- */

	/* ⭐ THIS ONE IS HERE SO THAT THE FOUR BELOW MEAN SOMETHING. Every criterion
	 * below asserts that a caller's redirection was carried across; without this
	 * one they would all pass for a library that had stopped letting a started
	 * program inherit anything at all. Its marker is the one continuous
	 * integration requires to be PRESENT in the log. */
	{
		pid_t pid = -1;
		char* av[] = { (char*)self, (char*)"--child-echo",
		               (char*)MARK_INHERIT, 0 };
		const int e = posix_spawn(&pid, self, NULL, NULL, av, environ);
		check(e == 0, "a program starts and inherits the streams of the caller");
		if (e == 0) wait_for(pid);
		printf("\n");
	}

	/* --- a redirection the caller performed, three ways it can be expressed ---- */

	/* ⭐ ONE ROUTE WAS FIXED AND THREE CALL SITES REACH IT. The seeding lives
	 * inside `__posix_spawn', so `posix_spawn', `execve' and `system' are all
	 * answered by one change --- and one change answering three entries is not
	 * three entries having a criterion. Each is asked separately. */
	{
		const char* sink = "redirect-spawn.tmp";
		int saved = -1, e = -1;
		pid_t pid = -1;
		if (redirect_stdout(sink, &saved) == 0) {
			char* av[] = { (char*)self, (char*)"--child-echo",
			               (char*)MARK_SPAWN, 0 };
			e = posix_spawn(&pid, self, NULL, NULL, av, environ);
			if (e == 0) wait_for(pid);
		}
		restore_stdout(saved);
		long got = -1;
		const int ok = (e == 0) && holds(sink, MARK_SPAWN, &got);
		if (!ok) printf("   posix_spawn: e=%d bytes in the sink=%ld (expected %u)\n",
		                e, got, (unsigned)strlen(MARK_SPAWN));
		check(ok, "posix_spawn carries a caller's redirection to the started program");
		unlink(sink);
	}

	if (expect_fork) {
		const char* sink = "redirect-execve.tmp";
		int saved = -1;
		pid_t p = -1;
		if (redirect_stdout(sink, &saved) == 0) {
			p = fork();
			if (p == 0) {
				char* av[] = { (char*)self, (char*)"--child-echo",
				               (char*)MARK_EXECVE, 0 };
				execv(self, av);
				_exit(127);
			}
			if (p > 0) wait_for(p);
		}
		restore_stdout(saved);
		long got = -1;
		const int ok = (p > 0) && holds(sink, MARK_EXECVE, &got);
		if (!ok) printf("   execve: pid=%d bytes in the sink=%ld (expected %u)\n",
		                (int)p, got, (unsigned)strlen(MARK_EXECVE));
		check(ok, "and so does replacing the calling image with another program");
		unlink(sink);
	}

	/* --- a file action that names the file itself ------------------------------ */

	/* ⭐ `addopen' WAS REFUSED AND IS EXPRESSIBLE. It is the ordinary way to send
	 * a started program's output to a file, and refusing it forced every caller
	 * through `dup2' --- which is the route that did not work. */
	{
		const char* sink = "redirect-addopen.tmp";
		posix_spawn_file_actions_t fa;
		pid_t pid = -1;
		int e = -1;
		if (posix_spawn_file_actions_init(&fa) == 0) {
			char* av[] = { (char*)self, (char*)"--child-echo",
			               (char*)MARK_ADDOPEN, 0 };
			if (posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, sink,
			                                     O_WRONLY | O_CREAT | O_TRUNC,
			                                     0666) == 0)
				e = posix_spawn(&pid, self, &fa, NULL, av, environ);
			posix_spawn_file_actions_destroy(&fa);
		}
		if (e == 0) wait_for(pid);
		long got = -1;
		const int ok = (e == 0) && holds(sink, MARK_ADDOPEN, &got);
		if (!ok) printf("   addopen: e=%d bytes in the sink=%ld (expected %u)\n",
		                e, got, (unsigned)strlen(MARK_ADDOPEN));
		check(ok, "a file action may name the file a started program writes to");
		unlink(sink);
	}

	/* --- and one that cannot be performed --------------------------------------- */

	/* ⭐ A REFUSAL IS A CRITERION. openkal has no value meaning "no stream", and
	 * the value that looks like one --- zero --- means the opposite: the stream
	 * the caller has. So closing one of the three in the started program cannot
	 * be done, and it used to be ACCEPTED and not done, which handed a program
	 * the standard input its caller had just taken away. */
	{
		posix_spawn_file_actions_t fa;
		pid_t pid = -1;
		int e = -1;
		if (posix_spawn_file_actions_init(&fa) == 0) {
			char* av[] = { (char*)self, (char*)"--child-exit", (char*)"0", 0 };
			if (posix_spawn_file_actions_addclose(&fa, STDOUT_FILENO) == 0)
				e = posix_spawn(&pid, self, &fa, NULL, av, environ);
			posix_spawn_file_actions_destroy(&fa);
		}
		if (e == 0) wait_for(pid);      /* it should not have started */
		check(e == ENOSYS,
		      "closing a standard stream in the started program is refused, not ignored");
	}

	/* --- how a program that ended abnormally is reported ------------------------ */

	/* ⚠️⚠️ `abort' DID NOT END THE PROGRAM AND AN ILLEGAL INSTRUCTION DID.
	 * musl's `raise' is `tkill', this port had no case for it, so `abort' fell
	 * through to the line musl's own comment calls unreachable --- `a_crash()',
	 * which on x86_64 is `hlt' and faults as a segmentation fault. Every uncaught
	 * exception, every `assert', every `std::terminate' over this port reported
	 * 139, and a consumer reading that was told it had a bad pointer. */
	{
		pid_t pid = -1;
		int st_abort = 0, st_plain = 0;
		char* av[] = { (char*)self, (char*)"--child-abort", 0 };
		int e = posix_spawn(&pid, self, NULL, NULL, av, environ);
		check(e == 0, "a program that ends itself abnormally can be started");
		if (e == 0) while (waitpid(pid, &st_abort, 0) < 0 && errno == EINTR) { }

		char* cv[] = { (char*)self, (char*)"--child-exit", (char*)"3", 0 };
		const int ce = posix_spawn(&pid, self, NULL, NULL, cv, environ);
		if (ce == 0) while (waitpid(pid, &st_plain, 0) < 0 && errno == EINTR) { }

		/* The control, and it is not decoration: without it the assertions above
		 * would hold for a library that reported every child the same way. */
		check(ce == 0 && WIFEXITED(st_plain) && WEXITSTATUS(st_plain) == 3,
		      "a program that returned a status reports that status");

		const int distinguishable =
			(e == 0) && !(WIFEXITED(st_abort) && WEXITSTATUS(st_abort) == 0)
			&& st_abort != st_plain;
		if (!distinguishable)
			printf("   abort: status=0x%x exited=%d code=%d signalled=%d signal=%d"
			       "   control status=0x%x\n",
			       (unsigned)st_abort, WIFEXITED(st_abort), WEXITSTATUS(st_abort),
			       WIFSIGNALED(st_abort), WTERMSIG(st_abort), (unsigned)st_plain);
		check(distinguishable,
		      "an abnormal end is distinguishable from every ordinary one");

		/* ⭐ AND THE PARTICULAR END THIS SYSTEM'S IMPLEMENTATION PRODUCES, which
		 * is what separates `abort reached kal_abort' from `abort fell through
		 * to an illegal instruction'. The observation above cannot: a fault is
		 * distinguishable from an ordinary end too. */
		int ok = 0;
		const char* what = "";
		switch (expect_abort) {
		case ABORT_SIGABRT:
			ok = WIFSIGNALED(st_abort) && WTERMSIG(st_abort) == SIGABRT;
			what = "and it is the signal a C library raises";
			break;
		case ABORT_STATUS_134:
			ok = WIFEXITED(st_abort) && WEXITSTATUS(st_abort) == 134;
			what = "and it is the status this system's implementation ends with";
			break;
		default:
			/* Any signal, and not one of the faults the defect produced. This
			 * system reports a fail-fast as a termination and reported the
			 * illegal instruction it replaces as an ordinary exit. */
			ok = WIFSIGNALED(st_abort)
			     && WTERMSIG(st_abort) != SIGSEGV && WTERMSIG(st_abort) != SIGILL;
			what = "and this system reports it as a termination, not as a return";
			break;
		}
		if (!ok)
			printf("   abort: status=0x%x exited=%d code=%d signalled=%d signal=%d\n",
			       (unsigned)st_abort, WIFEXITED(st_abort), WEXITSTATUS(st_abort),
			       WIFSIGNALED(st_abort), WTERMSIG(st_abort));
		check(ok, what);
	}

	/* --- asking after a started program without waiting for it ------------------ */

	/* ⚠️ `waitpid' DISCARDED ITS OPTIONS, so the one call whose purpose is not to
	 * wait blocked until the program finished. `kal_timeout_wait_process' has
	 * been in the specification since 0.8; the route was missing. */
	{
		pid_t pid = -1;
		char* av[] = { (char*)self, (char*)"--child-sleep", (char*)"400", 0 };
		const int e = posix_spawn(&pid, self, NULL, NULL, av, environ);
		check(e == 0, "a program that takes a while to finish can be started");
		if (e == 0) {
			int st = 0;
			long spins = 0;
			pid_t r;
			while ((r = waitpid(pid, &st, WNOHANG)) == 0 && spins < 100000) spins++;
			/* ⭐ THE CRITERION IS THAT THE CALLER GOT CONTROL BACK, which is
			 * what a count greater than zero states and what a count of exactly
			 * one would not distinguish from having blocked once. */
			if (spins == 0)
				printf("   WNOHANG returned %d on the first call; the program had"
				       " already finished or the call blocked\n", (int)r);
			check(spins > 0, "asking without waiting returns while it is still running");
			check(r == pid, "and reports it once it has finished");
			check(WIFEXITED(st) && WEXITSTATUS(st) == 7,
			      "with the status that program was written to report");
		} else {
			failures += 3;
		}
	}

	/* ⭐ AND ANY OF THEM, NOT THE FIRST ONE RECORDED.
	 *
	 * `waitpid(-1, …, WNOHANG)` asks after any child. Asking after the first
	 * recorded one would report "none has finished" while a later one had, which
	 * is the reading a caller draining its children in a loop acts upon. The
	 * slow program is started FIRST so that it occupies the earlier slot: a
	 * library that asked only about that one would answer zero here until the
	 * slow one finished, and would then name the wrong program. */
	{
		pid_t slow = -1, quick = -1;
		char* sv[] = { (char*)self, (char*)"--child-sleep", (char*)"600", 0 };
		char* qv[] = { (char*)self, (char*)"--child-sleep", (char*)"1", 0 };
		const int es = posix_spawn(&slow,  self, NULL, NULL, sv, environ);
		const int eq = posix_spawn(&quick, self, NULL, NULL, qv, environ);
		check(es == 0 && eq == 0, "two programs can be running at once");
		if (es == 0 && eq == 0) {
			int st = 0;
			long spins = 0;
			pid_t r;
			while ((r = waitpid(-1, &st, WNOHANG)) == 0 && spins < 100000) spins++;
			if (r != quick)
				printf("   waitpid(-1, WNOHANG) reported %d; the one that finished"
				       " first was %d and the other was %d\n",
				       (int)r, (int)quick, (int)slow);
			check(r == quick,
			      "asking after any of them names the one that finished first");
			wait_for(slow);
		} else {
			failures += 1;
		}
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

	{
		const char* sink = "redirect-system.tmp";
		int saved = -1, r = -1;
		if (redirect_stdout(sink, &saved) == 0)
			r = system("echo " MARK_SHELL);
		restore_stdout(saved);
		long got = -1;
		const int ok = (r >= 0) && WIFEXITED(r) && WEXITSTATUS(r) == 0
		             && holds(sink, MARK_SHELL, &got);
		if (!ok) printf("   system: rc=%d bytes in the sink=%ld (expected %u)\n",
		                r, got, (unsigned)strlen(MARK_SHELL));
		check(ok, "and a command line writes where the caller had redirected");
		unlink(sink);
	}

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

	/* --- what a copy of this image calls itself -------------------------------- */

	if (expect_fork) {
		/* ⚠️⚠️ EVERY CONTEXT USED TO ANSWER 1, SO A COPY REPORTED THE IDENTIFIER
		 * OF THE IMAGE IT WAS COPIED FROM. Two contexts, one answer, and no way
		 * for the copy to name itself --- a program writing its own identifier
		 * where something else would read it (a lock file, the name of a
		 * temporary, a line of a log) wrote a value naming something else.
		 *
		 * ⭐ THE NUMBER BOTH SIDES ALREADY AGREE ON is the one `fork' returns to
		 * the parent, so that is what the copy is told. It has to exist BEFORE
		 * the copy is taken, which is why okm_fork.c reserves the table entry
		 * above `kal_space_start' rather than recording it below. */
		int tell[2];
		check(pipe(tell) == 0, "a copy can be asked what it calls itself");
		const pid_t named = fork();
		if (named == 0) {
			char m[32];
			const int n = snprintf(m, sizeof m, "%ld", (long)getpid());
			(void)!write(tell[1], m, (size_t)(n > 0 ? n : 0));
			_exit(0);
		}
		close(tell[1]);
		char got[32];
		memset(got, 0, sizeof got);
		(void)!read(tell[0], got, sizeof got - 1);
		close(tell[0]);
		int st_named = 0;
		waitpid(named, &st_named, 0);
		const long said = atol(got);
		if (said != (long)named)
			printf("   the parent was given %ld and the copy said %ld\n",
			       (long)named, said);
		check(named > 0 && said == (long)named,
		      "and it names the identifier its parent was given, not its parent's");

		/* ⚠️ THE GUARD THIS CHANGE NEEDS, AND IT IS NOT A CRITERION --- it holds
		 * before the change as well. `kill' decides "this program itself" by
		 * comparing against the identifier, and that comparison was against the
		 * constant 1. Carrying an identifier into a copy without moving the
		 * comparison would make `raise' --- and therefore `abort', and therefore
		 * every uncaught exception --- report ESRCH in every copy. */
		const pid_t dying = fork();
		if (dying == 0) abort();
		int st_dying = 0;
		waitpid(dying, &st_dying, 0);
		int copy_ok = 0;
		switch (expect_abort) {
		case ABORT_SIGABRT:    copy_ok = WIFSIGNALED(st_dying)
		                              && WTERMSIG(st_dying) == SIGABRT; break;
		case ABORT_STATUS_134: copy_ok = WIFEXITED(st_dying)
		                              && WEXITSTATUS(st_dying) == 134; break;
		default:               copy_ok = WIFSIGNALED(st_dying); break;
		}
		if (!copy_ok)
			printf("   abort in a copy: status=0x%x exited=%d code=%d signalled=%d\n",
			       (unsigned)st_dying, WIFEXITED(st_dying), WEXITSTATUS(st_dying),
			       WIFSIGNALED(st_dying));
		check(copy_ok, "and a copy that ends itself abnormally still does so");
	}

	/* --- a program that CANNOT be started ------------------------------------- */

	/* ⭐⭐ THE QUESTION THIS FILE NEVER ASKED, AND THE ONE A CONSUMER LOST NINE
	 * TESTS TO.
	 *
	 * Every observation above starts a program that is there. None asked what
	 * happens when the name names nothing --- and the answer was that
	 * `kal_process_spawn' duplicates and replaces, so the replacement fails
	 * INSIDE THE DUPLICATE, which ends with 127 and tells nobody. `posix_spawn'
	 * reported success. `execve' waited for the duplicate, read 127, and ENDED
	 * THE CALLING PROGRAM with it.
	 *
	 * ⚠️ WHICH BREAKS EVERY SEARCH FOR A PROGRAM BY NAME. musl's `execvp'
	 * issues one `execve' per PATH entry and needs it to RETURN so it can try
	 * the next; here it did not return at all, so the first entry that missed
	 * was the end. A consumer measured `bwrap' --- installed at /usr/bin/bwrap
	 * --- being reported as not installed. */

	errno = 0;
	{
		pid_t absent_pid = -1;
		char* av[] = { (char*)"no-such-program", NULL };
		char* ev[] = { NULL };
		const int e = posix_spawn(&absent_pid, "/no-such-program-here", NULL, NULL,
		                          av, ev);
		check(e == ENOENT,
		      "starting a program that is not there reports that, rather than success");
		if (e == 0) { int s; waitpid(absent_pid, &s, 0); failures++; }
	}

	{
		/* POSIX names this one separately, and `execvp' continues its search on
		 * it exactly as it does on ENOENT. */
		pid_t dir_pid = -1;
		char* av[] = { (char*)".", NULL };
		char* ev[] = { NULL };
		const int e = posix_spawn(&dir_pid, ".", NULL, NULL, av, ev);
		check(e == EACCES, "and naming a directory is refused as a directory");
		if (e == 0) { int s; waitpid(dir_pid, &s, 0); failures++; }
	}

	/* ⚠️ CALLED IN THIS PROGRAM AND NOT IN A COPY, DELIBERATELY. What is being
	 * observed is that `execve' RETURNS; a version that does not return ends
	 * this program at 127, and the probe runner reports a program that stopped
	 * without a count of failures --- which is the loudest reading available and
	 * is the correct one, because a caller of `execvp' cannot survive it either. */
	{
		char* av[] = { (char*)"no-such-program", NULL };
		char* ev[] = { NULL };
		errno = 0;
		const int r = execve("/no-such-program-here", av, ev);
		check(r == -1 && errno == ENOENT,
		      "replacing this image with a program that is not there returns, with a reason");
	}

	/* --- and a program found by searching a PATH ------------------------------ */

	if (expect_shell) {
		/* The first entry misses. That is the whole point: the search has to
		 * survive it, and until now it could not. */
		/* ⚠️ COPIED WITH A LENGTH THAT IS THE VALUE'S RATHER THAN A BUFFER'S.
		 * `getenv' answers a pointer INTO the environment and `setenv' below may
		 * move it, so the old value has to be kept somewhere --- and a fixed
		 * buffer would silently truncate on a machine whose PATH is long, which
		 * continuous-integration machines are. A truncated PATH restored at the
		 * end is a probe quietly corrupting the environment of whatever it adds
		 * next. */
		const char* saved_path = getenv("PATH");
		char* keep = NULL;
		if (saved_path) {
			keep = malloc(strlen(saved_path) + 1);
			check(keep != NULL, "the PATH this program was started with can be kept");
			if (keep) strcpy(keep, saved_path);
		}
		setenv("PATH", "/no-such-directory:/bin:/usr/bin", 1);

		pid_t sp = -1;
		char* av[] = { (char*)"sh", (char*)"-c", (char*)"exit 23", NULL };
		const int e = posix_spawnp(&sp, "sh", NULL, NULL, av, environ);
		check(e == 0, "a program named without a path is found by searching PATH");
		if (e == 0) {
			int s = 0;
			check(waitpid(sp, &s, 0) == sp && WIFEXITED(s) && WEXITSTATUS(s) == 23,
			      "and it is the program that was searched for");
		} else {
			failures++;
		}

		if (expect_fork) {
			/* The same search, through the route musl writes it: one `execve'
			 * per entry, in a copy, relying on each one returning. */
			const pid_t vp = fork();
			if (vp == 0) {
				char* cav[] = { (char*)"sh", (char*)"-c", (char*)"exit 29", NULL };
				execvp("sh", cav);
				_exit(97);        /* the search gave up: 97, not 127 */
			}
			int s = 0;
			check(vp > 0 && waitpid(vp, &s, 0) == vp
			      && WIFEXITED(s) && WEXITSTATUS(s) == 29,
			      "and the same search performed by execvp survives its first miss");
		}

		if (keep) { setenv("PATH", keep, 1); free(keep); }
		else      unsetenv("PATH");
	}

	printf("-- failures: %d --\n", failures);
	return failures ? 1 : 0;
}
