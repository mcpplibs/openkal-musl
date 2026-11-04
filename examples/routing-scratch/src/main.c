/* Routing scratch is needed by path mutations and file copies, rather than by
 * every system call. Exercise those operations and their refusals, compare the
 * null-name fast rejection with full lexical resolution, and release channel
 * descriptions concurrently. The same image runs on all four CI hosts. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* The port's full resolver is an independent oracle for the fast rejection:
 * this test does not reproduce its component-normalisation algorithm. */
extern int okm_null_name(int, const char*);
extern int okm_absolute(int, const char*, char*, size_t);

static int failures;
static void check(int ok, const char* what)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) failures++;
}

static void null_names(int dirfd)
{
	static const char* prefix[] = { "", "/", "/dev/", "/dev/other/../",
		"/dev/null/", "../", "C:/dev/", "c:\\dev\\", "//dev//",
		"/dev/null\\", "/dev/null/../", "/dev/null/x/../../dev/" };
	static const char* component[] = { "null", ".", "..", "ordinary", "nul",
		"Null", "null\\", "nullx", "null/../null" };
	static const char* suffix[] = { "", "/", "//", "/.", "/..", "\\", "\\.",
		"/../null", "/ordinary" };
	const size_t np = sizeof prefix / sizeof *prefix;
	const size_t nc = sizeof component / sizeof *component;
	const size_t ns = sizeof suffix / sizeof *suffix;
	int mismatches = 0;
	for (size_t i = 0; i < np * nc * ns; i++) {
		char path[256], absolute[4096];
		snprintf(path, sizeof path, "%s%s%s", prefix[i / (nc * ns)],
		         component[(i / ns) % nc], suffix[i % ns]);
		const int expected = okm_absolute(dirfd, path, absolute, sizeof absolute) == 0
		                   && !strcmp(absolute, "/dev/null");
		if (okm_null_name(dirfd, path) != expected) {
			if (!mismatches) printf("first null-name mismatch: dirfd=%d %s\n", dirfd, path);
			mismatches++;
		}
	}
	check(!mismatches, "null-name rejection agrees with full resolution for 972 spellings");
	char long_name[8192], absolute[4096];
	int long_mismatches = 0;
	for (size_t length = 4094; length <= 4097; length++) {
		memset(long_name, '/', length);
		memcpy(long_name + length - 8, "dev/null", 8);
		long_name[length] = 0;
		const int expected = okm_absolute(dirfd, long_name, absolute, sizeof absolute) == 0
		                   && !strcmp(absolute, "/dev/null");
		if (okm_null_name(dirfd, long_name) != expected) long_mismatches++;
	}
	check(!long_mismatches, "null-name rejection preserves the full resolver's path-length boundary");
}

static void null_device(void)
{
	check(!okm_null_name(AT_FDCWD, 0) && !okm_null_name(AT_FDCWD, ""),
	      "null and empty names are not the null device");
	const int fd = open("/dev/./missing/../null/.", O_RDWR);
	char byte = 'x';
	check(fd >= 0 && write(fd, &byte, 1) == 1 && read(fd, &byte, 1) == 0,
	      "the normalised null device consumes writes and reports end of input");
	if (fd >= 0) close(fd);
}

static void mutations(int dirfd)
{
	check(mkdirat(dirfd, "nested", 0700) == 0, "mkdirat resolves against a directory descriptor");
	const int input = openat(dirfd, "source", O_CREAT | O_RDWR | O_TRUNC, 0600);
	const int output = openat(dirfd, "dest", O_CREAT | O_RDWR | O_TRUNC, 0600);
	char data[10017], copied[10017];
	for (size_t i = 0; i < sizeof data; i++) data[i] = (char)(i % 127);
	check(input >= 0 && output >= 0 && write(input, data, sizeof data) == sizeof data,
	      "the copy source contains more than one scratch block");
	if (input < 0 || output < 0) { if (input >= 0) close(input); if (output >= 0) close(output); return; }
	check(lseek(input, 3, SEEK_SET) == 3 && lseek(output, 7, SEEK_SET) == 7,
	      "the descriptors have independent current positions");
	off_t in = 0, out = 0;
	check(copy_file_range(input, &in, output, &out, sizeof data, 0) == sizeof data
	      && in == sizeof data && out == sizeof data
	      && lseek(input, 0, SEEK_CUR) == 3 && lseek(output, 0, SEEK_CUR) == 7,
	      "copy_file_range updates explicit offsets and preserves descriptor positions");
	check(pread(output, copied, sizeof copied, 0) == sizeof copied && !memcmp(data, copied, sizeof data),
	      "the copied bytes agree across the scratch boundary");
	errno = 0;
	check(copy_file_range(input, 0, output, 0, 1, 1) == -1 && errno == EINVAL,
	      "undefined copy flags are refused");
	close(input); close(output);
#ifdef SYS_renameat2
	errno = 0;
	check(syscall(SYS_renameat2, dirfd, "source", dirfd, "dest", 2) == -1 && errno == EINVAL,
	      "exchange is refused without replacing either name");
	check(faccessat(dirfd, "source", F_OK, 0) == 0 && faccessat(dirfd, "dest", F_OK, 0) == 0,
	      "both names remain after the refusal");
#endif
	check(renameat(dirfd, "source", dirfd, "renamed") == 0,
	      "renameat resolves both names against the descriptor");
	check(truncate("renamed", 1021) == 0, "truncate resolves the name in the working directory");
	struct stat st;
	check(fstatat(dirfd, "renamed", &st, 0) == 0 && st.st_size == 1021,
	      "the named file has the requested length");
	struct timespec times[2] = { { 1700000000, 0 }, { 1700000001, 0 } };
	check(utimensat(dirfd, "renamed", times, 0) == 0
	      && fstatat(dirfd, "renamed", &st, 0) == 0 && st.st_mtime == 1700000001,
	      "utimensat resolves the name and sets the modification time");
	struct statvfs capacity;
	check(statvfs(".", &capacity) == 0 && capacity.f_bsize > 0,
	      "the filesystem capacity resolves its name");
	check(unlinkat(dirfd, "renamed", 0) == 0 && unlinkat(dirfd, "dest", 0) == 0
	      && unlinkat(dirfd, "nested", AT_REMOVEDIR) == 0,
	      "unlinkat removes both files and the directory");
}

static void* channels(void* arg)
{
	int* bad = arg;
	for (int i = 0; i < 100; i++) {
		int ends[2];
		if (pipe2(ends, O_NONBLOCK) != 0) { (*bad)++; break; }
		const int alias = dup(ends[1]);
		close(ends[1]);
		char byte = 'q';
		if (alias < 0 || write(alias, &byte, 1) != 1 || read(ends[0], &byte, 1) != 1 || byte != 'q') (*bad)++;
		errno = 0;
		if (read(ends[0], &byte, 1) != -1 || errno != EAGAIN) (*bad)++;
		if (alias >= 0) close(alias);
		if (read(ends[0], &byte, 1) != 0) (*bad)++;
		close(ends[0]);
	}
	return 0;
}

int main(void)
{
	null_names(AT_FDCWD);
	null_names(-123);
	null_device();
	char original[4096], temp[] = "okm-routing-XXXXXX";
	check(getcwd(original, sizeof original) && mkdtemp(temp) && chdir(temp) == 0,
	      "the probe owns a temporary working directory");
	if (failures) return 1;
	const int dirfd = open(".", O_RDONLY | O_DIRECTORY);
	check(dirfd >= 0, "the temporary directory has a descriptor");
	if (dirfd >= 0) { null_names(dirfd); mutations(dirfd); close(dirfd); }
	pthread_t threads[4];
	int bad[4] = { 0 }, started = 0;
	for (int i = 0; i < 4; i++) {
		if (pthread_create(&threads[i], 0, channels, &bad[i])) break;
		started++;
	}
	for (int i = 0; i < started; i++) pthread_join(threads[i], 0);
	check(started == 4 && !bad[0] && !bad[1] && !bad[2] && !bad[3],
	      "400 concurrent channel lifetimes retain aliases and close before returning");
	check(chdir(original) == 0 && rmdir(temp) == 0, "the owned temporary directory is removed");
	printf("-- failures: %d --\n", failures);
	return failures != 0;
}
