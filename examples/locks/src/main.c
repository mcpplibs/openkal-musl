/* A lock held by the open file: flock, and fcntl's open-file-description
 * forms, over openkal's kal_fs_lock (port/src/okm_syscall.c).
 *
 * THE OTHER HOLDER IS THE OBSERVATION. A call that reported success is what a
 * port that took no lock would also give, so every lock is tried again from a
 * second open of the same file, which is another holder: openkal states the
 * holder as the open file, as Linux's open-file-description locks and BSD's
 * flock do. The second open is refused while the first holds, and admitted once
 * the first releases, closes, or holds only a shared lock beside it.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/file.h>
#include <unistd.h>

static int failures;
static void check(int ok, const char* what)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) failures++;
}

static int ofd(int fd, int type)
{
	struct flock r = {0};
	r.l_type = type;
	r.l_whence = SEEK_SET;
	return fcntl(fd, F_OFD_SETLK, &r);
}

int main(void)
{
	const char* name = "okm-locks-probe.tmp";
	int a = open(name, O_RDWR | O_CREAT | O_TRUNC, 0644);
	int b = open(name, O_RDWR);
	check(a >= 0 && b >= 0, "a file is opened twice");
	if (a < 0 || b < 0) return 1;

	check(flock(a, LOCK_EX | LOCK_NB) == 0, "flock: the first open takes the file exclusively");
	errno = 0;
	check(flock(b, LOCK_EX | LOCK_NB) == -1 && errno == EWOULDBLOCK, "flock: the second open is refused, EWOULDBLOCK");
	check(flock(b, LOCK_SH | LOCK_NB) == -1, "flock: and refused a shared lock");
	check(flock(a, LOCK_UN) == 0, "flock: the first releases");
	check(flock(b, LOCK_SH | LOCK_NB) == 0 && flock(a, LOCK_SH | LOCK_NB) == 0, "flock: two shared holders");
	check(flock(a, LOCK_EX | LOCK_NB) == -1, "flock: no exclusive lock beside a shared one");
	check(flock(a, LOCK_UN) == 0 && flock(b, LOCK_UN) == 0, "flock: both release");
	errno = 0;
	check(flock(a, 0) == -1 && errno == EINVAL, "flock: an operation that is none is refused, EINVAL");

	check(ofd(a, F_WRLCK) == 0, "F_OFD_SETLK: the first open takes a write lock");
	errno = 0;
	check(ofd(b, F_WRLCK) == -1 && (errno == EAGAIN || errno == EACCES), "F_OFD_SETLK: the second open is refused");
	close(a);
	check(ofd(b, F_WRLCK) == 0, "F_OFD_SETLK: closing the first open releases its lock");
	check(ofd(b, F_UNLCK) == 0, "F_OFD_SETLK: the second releases");
	close(b);
	unlink(name);
	printf("-- failures: %d --\n", failures);
	return failures != 0;
}
