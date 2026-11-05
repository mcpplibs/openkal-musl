/* The optional operation's compatibility and error contract, compiled with
 * the actual port source. These stubs do not implement a filesystem. */
#include "okm.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

long okm_test_realpath(const char *, char *, size_t);
static int failures, queries, resolves, calls;
static int mode, route_error;
static void check(int yes, const char *name)
{
	if (!yes) { fprintf(stderr, "FAIL: %s\n", name); failures++; }
}
int okm_null_name(int dirfd, const char *name)
{
	(void)dirfd;
	return strcmp(name, "/dev/null") == 0;
}
int okm_resolve(int dirfd, const char *name, struct okm_at *at, int empty_ok)
{
	(void)dirfd; (void)empty_ok;
	resolves++;
	if (route_error) return route_error;
	at->base.h = 42;
	at->len = strlen(name);
	memcpy(at->rel, name, at->len + 1);
	return 0;
}
int okm_errno(int error)
{
	if (error == kal_err_permission) return EACCES;
	if (error == kal_err_not_found) return ENOENT;
	return EIO;
}
kal_uintptr kal_fs_props(struct kal_dir base)
{
	(void)base;
	queries++;
	return KAL_FS_PROP_RESOLVED_NAME;
}
#if !REALPATH_ABSENT
kal_intptr kal_fs_resolved_name(struct kal_dir base, const char *name,
                                kal_uintptr len, char *out, kal_uintptr cap)
{
	calls++;
	check(base.h == 42 && len == strlen(name), "resolved directory and name");
	if (mode) return -(kal_intptr)mode;
	const char answer[] = "/actual/file";
	const size_t n = sizeof answer - 1;
	if (out) memcpy(out, answer, n < cap ? n : cap);
	return (kal_intptr)n;
}
#endif
int main(void)
{
	char out[64];
	check(okm_test_realpath(0, out, sizeof out) == 0, "null stays on POSIX path");
	check(okm_test_realpath("a/../b", out, sizeof out) == 0, "parent component stays on POSIX path");
	check(okm_test_realpath("./file", out, sizeof out) == 0, "dot component stays on POSIX path");
	check(okm_test_realpath("file/", out, sizeof out) == 0, "trailing separator stays on POSIX path");
	check(resolves == 0 && calls == 0, "lexical fallbacks call no optional operation");
	check(okm_test_realpath("/dev/null", out, sizeof out) == 9
	      && strcmp(out, "/dev/null") == 0, "null device remains special");
#if REALPATH_ABSENT
	check(okm_test_realpath("file", out, sizeof out) == 0, "old runner falls back");
	check(resolves == 0 && calls == 0, "old runner needs no route or optional entry");
#else
	check(okm_test_realpath("file", out, sizeof out) == 12
	      && strcmp(out, "/actual/file") == 0, "resolved name and terminator");
	mode = kal_err_not_supported;
	check(okm_test_realpath("file", out, sizeof out) == 0, "unsupported falls back");
	mode = kal_err_permission;
	check(okm_test_realpath("file", out, sizeof out) == -EACCES, "permission is preserved");
	mode = kal_err_not_found;
	check(okm_test_realpath("file", out, sizeof out) == -ENOENT, "absence is preserved");
	mode = 0;
	check(okm_test_realpath("file", out, 2) == -ENAMETOOLONG, "short output is refused");
	const int before = calls;
	route_error = -EACCES;
	check(okm_test_realpath("file", out, sizeof out) == 0 && calls == before,
	      "failed route stays on POSIX path");
#endif
	check(queries == 0, "the operation does not require a property preflight");
	printf("realpath optional operation: %s\n", failures ? "FAIL" : "PASS");
	return failures != 0;
}
