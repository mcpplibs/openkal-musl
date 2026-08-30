/* The surface a program reaches for that is neither a file nor another program,
 * asked by a program that names no openkal symbol.
 *
 * ⭐⭐ WHY THIS FILE EXISTS, AND IT IS THE WHOLE POINT OF IT.
 *
 * Every probe in this repository until now asked whether something WORKED. None
 * of them asked whether an answer was TRUE. Those are different questions, and
 * the difference is where a whole family of defects lived:
 *
 *   fcntl(F_SETLK)   granted every lock and took none, so two programs held one
 *                    exclusive lock and neither could find out
 *   fcntl(F_GETLK)   left the caller's word untouched, so the answer read
 *                    "somebody holds this" --- for ever, and a loop waiting for
 *                    a lock to be released never left it
 *   getppid()        returned -38, a negated error value, AS A PROCESS
 *                    IDENTIFIER, with errno untouched
 *   sigaltstack()    reported an installation it had not performed, and the
 *                    enquiry that would have caught it lied in the same
 *                    direction
 *   sysconf(_SC_OPEN_MAX)
 *                    answered 0 --- a bound this library sets itself and was
 *                    refusing to state
 *
 * ⚠️ NOT ONE of those is a missing operation, so not one of them appears in
 * `OPENKAL_MUSL_TRACE=enosys'. They are operations that are PRESENT AND ANSWER
 * WRONGLY, which no diagnostic here can see and no probe here was asking about.
 * They were found by writing this file and comparing every answer against the
 * host's, and that comparison is what the file preserves.
 *
 * ⚠️ SO EVERY OBSERVATION BELOW STATES THE ANSWER IT EXPECTS, and a refusal is
 * an expected answer wherever a refusal is the truth. "It returned" is not an
 * observation this file makes.
 *
 *   --dir-time | --no-dir-time   whether this system can set the modification
 *                                time of a DIRECTORY. openkal states
 *                                `kal_fs_set_modified' on an open FILE and has
 *                                no `kal_dir' form, so the port opens the
 *                                directory for reading and sets it --- which
 *                                Linux and macOS perform and an environment
 *                                that cannot open a directory at all does not.
 *                                Recorded in musl/PATCHES.md.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int failures = 0;

static void check(int ok, const char* what)
{
	if (!ok) { printf("FAIL: %s (errno=%d)\n", what, errno); failures++; }
	else printf("ok: %s\n", what);
}

/* A call that must fail, and must fail WITH A PARTICULAR REASON.
 *
 * ⚠️ THE REASON IS HALF THE OBSERVATION. `setsid' answering ENOSYS and `setsid'
 * answering EPERM are both refusals and are not the same answer: every
 * daemonising library in existence handles the second, because the
 * `fork'-then-`setsid' dance exists for it, and none handles the first. */
static void refuses(int r, int want, const char* what)
{
	if (r != -1)        { printf("FAIL: %s (it succeeded)\n", what); failures++; }
	else if (errno != want) {
		printf("FAIL: %s (refused with errno=%d, wanted %d)\n", what, errno, want);
		failures++;
	} else printf("ok: %s\n", what);
}

int main(int argc, char** argv)
{
	int expect_dir_time = -1;
	for (int i = 1; i < argc; i++) {
		if      (strcmp(argv[i], "--dir-time")    == 0) expect_dir_time = 1;
		else if (strcmp(argv[i], "--no-dir-time") == 0) expect_dir_time = 0;
	}
	if (expect_dir_time < 0) {
		printf("usage: surface --dir-time|--no-dir-time\n");
		return 2;
	}

	/* --- an identifier is a number, and never a negated error -------------- */

	/* ⚠️⚠️ THE FAMILY AND NOT THE MEMBER. `getpgrp' had exactly this defect and
	 * was fixed; `getppid', three lines away in the same dispatch, was not
	 * looked for and had it too. Both are written in musl WITHOUT
	 * `__syscall_ret' --- deliberately, because POSIX says they cannot fail ---
	 * so an unhandled number is handed to the caller as the answer. There is
	 * nothing to check and no errno to read: the value simply is -38.
	 *
	 * ⇒ This asks the whole family at once, so that the next member added is
	 * covered by an observation that already exists. */
	const long ids[] = { getpid(), getppid(), getpgrp(), getpgid(0), getsid(0) };
	static const char* const id_names[] =
		{ "getpid", "getppid", "getpgrp", "getpgid(0)", "getsid(0)" };
	int all_ids_are_numbers = 1;
	for (unsigned i = 0; i < sizeof ids / sizeof *ids; i++) {
		if (ids[i] >= 0) continue;
		printf("FAIL: %s answered %ld, which is a negated error value\n",
		       id_names[i], ids[i]);
		all_ids_are_numbers = 1 - 1;
		failures++;
	}
	if (all_ids_are_numbers)
		printf("ok: every identifier this library reports is a number\n");

	/* --- a bound this library sets is a bound it can state ----------------- */

	const long open_max = sysconf(_SC_OPEN_MAX);
	check(open_max > 0, "the greatest number of descriptors is answered");

	/* ⚠️⚠️ IT ANSWERED 1, SILENTLY. `hardware_concurrency()' reads this, so a
	 * program sizing a pool of workers got one worker and no error. openkal 0.10
	 * added the enquiry beneath it. This asserts only that it is a real count
	 * and not the fallback, because the number itself is the machine's. */
	const long cpus = sysconf(_SC_NPROCESSORS_ONLN);
	check(cpus > 0, "how many contexts can run at once is answered");

	struct rlimit rl;
	const int got = getrlimit(RLIMIT_NOFILE, &rl);
	check(got == 0 && rl.rlim_cur > 0, "and the limit it is read from is the same enquiry");
	check(got == 0 && (long)rl.rlim_cur == open_max,
	      "and the two agree");

	/* --- a lock that is not taken is not reported as taken ----------------- */

	/* ⚠️⚠️ THESE THREE ANSWERED 0 AND DID NOTHING. Measured against the host:
	 * two programs took one exclusive lock and BOTH were told they had it.
	 *
	 * 0.10.0 refused them and said the refusal was TEMPORARY in a way `chmod'
	 * is not --- every environment beneath openkal can lock a byte range, and
	 * what was missing was a word in the specification. That comment said this
	 * observation was the one that would change when the word arrived, and that
	 * it should change to "a second holder is refused" rather than be deleted.
	 *
	 * ⭐ openkal 0.10 IS THAT WORD, so it changed, and this is now the
	 * observation it said it would become. `kal_fs_lock' states the holder as
	 * the open FILE, which is why a SECOND open file of one name is refused
	 * here --- the older process-held form would have granted it, and a library
	 * that opened one file twice would have destroyed its own lock. */
	{
		const int fd = open("surface.lock", O_RDWR | O_CREAT | O_TRUNC, 0644);
		check(fd >= 0, "a file to ask about locking can be made");
		if (fd >= 0) {
			struct flock fl;
			memset(&fl, 0, sizeof fl);
			fl.l_type = F_WRLCK; fl.l_whence = SEEK_SET;
			fl.l_start = 0; fl.l_len = 0;
			errno = 0;
			check(fcntl(fd, F_SETLK, &fl) == 0, "an exclusive lock is taken");

			/* ⭐ THE OBSERVATION THAT TELLS THE TWO FORMS APART, and it needs no
			 * second program: a SECOND OPEN FILE of the same name, here. The
			 * process-held form grants this, because the holder is the process
			 * and the process already holds it. The open-file form refuses it,
			 * and openkal states the open-file form. */
			const int again = open("surface.lock", O_RDWR);
			check(again >= 0, "the same name can be opened a second time");
			if (again >= 0) {
				struct flock two;
				memset(&two, 0, sizeof two);
				two.l_type = F_WRLCK; two.l_whence = SEEK_SET;
				two.l_start = 0; two.l_len = 0;
				errno = 0;
				refuses(fcntl(again, F_SETLK, &two), EAGAIN,
				        "and a second OPEN FILE is refused, not granted");
				close(again);
			}

			struct flock un;
			memset(&un, 0, sizeof un);
			un.l_type = F_UNLCK; un.l_whence = SEEK_SET; un.l_start = 0; un.l_len = 0;
			check(fcntl(fd, F_SETLK, &un) == 0, "the lock is released");

			/* ⚠️ AND THE ENQUIRY IS STILL REFUSED, WHICH IS NOT AN OVERSIGHT.
			 * `F_GETLK' asks whether a lock WOULD block without taking one, and
			 * openkal has no operation that answers a question without
			 * performing it. Taking the lock and releasing it would answer, and
			 * would also take a lock the caller did not ask for. */
			struct flock q;
			memset(&q, 0, sizeof q);
			q.l_type = F_WRLCK; q.l_whence = SEEK_SET; q.l_start = 0; q.l_len = 0;
			errno = 0;
			refuses(fcntl(fd, F_GETLK, &q), ENOSYS,
			        "but asking WHETHER one would block is still refused");
			close(fd);
		}
		unlink("surface.lock");
	}

	/* --- an installation that did not happen is not reported ---------------- */

	/* This library delivers no signals, so there is nowhere for a handler to run
	 * and nothing for an alternate stack to be. It used to answer 0 and install
	 * nothing --- and the enquiry answered 0 with a zeroed record, so a caller
	 * checking could not tell either. */
	{
		static char alt[65536];
		stack_t ss;
		memset(&ss, 0, sizeof ss);
		ss.ss_sp = alt; ss.ss_size = sizeof alt; ss.ss_flags = 0;
		errno = 0;
		refuses(sigaltstack(&ss, NULL), ENOSYS,
		        "an alternate signal stack is refused rather than pretended");
	}

	/* --- a group of one is a group this program is already in --------------- */

	/* ⚠️ BOTH OF THESE USED TO ANSWER ENOSYS, on the ground that "making a group
	 * is not the same as being in one" --- which answers a question neither of
	 * them asks. `setpgid(0, 0)' asks for the calling program to be in a group
	 * of its own, and `getpgid(0) == getpid()' above says it already is. */
	errno = 0;
	check(setpgid(0, 0) == 0,
	      "asking to be in a group of one succeeds, because it already holds");

	/* And the failure POSIX writes down for a caller that already leads a group,
	 * which is the state this library reports. A written-down failure is one a
	 * caller can act upon; ENOSYS is one nothing handles. */
	errno = 0;
	refuses(setsid(), EPERM,
	        "and starting a session reports the failure POSIX names for that state");

	/* --- the modification time of a directory ------------------------------ */

	/* Reported by a consumer as a failure to READ one. Reading was never broken:
	 * the C++ library above reports both directions of `last_write_time' under
	 * that one name, and it was setting that could not be performed. */
	{
		const char* d = "surface.dir";
		rmdir(d);
		check(mkdir(d, 0700) == 0, "a directory can be made");

		struct timespec ts[2];
		ts[0].tv_sec = 0; ts[0].tv_nsec = UTIME_OMIT;
		ts[1].tv_sec = 1700000000; ts[1].tv_nsec = 0;

		errno = 0;
		const int set = utimensat(AT_FDCWD, d, ts, 0);
		if (expect_dir_time) {
			check(set == 0, "the modification time of a directory can be set");
			struct stat st;
			check(stat(d, &st) == 0 && st.st_mtime == 1700000000,
			      "and reading it back answers what was set");
		} else {
			check(set == -1, "setting the modification time of a directory is refused here");
		}

		/* The control: whatever the answer above, a FILE still works, and it
		 * still asks for exactly what the interface requires. */
		const char* f = "surface.file";
		const int fd = open(f, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd >= 0) close(fd);
		errno = 0;
		check(utimensat(AT_FDCWD, f, ts, 0) == 0,
		      "the modification time of a file can be set");
		struct stat fst;
		check(stat(f, &fst) == 0 && fst.st_mtime == 1700000000,
		      "and reading that back answers what was set");
		unlink(f);
		rmdir(d);
	}

	printf("-- failures: %d --\n", failures);
	return failures ? 1 : 0;
}
