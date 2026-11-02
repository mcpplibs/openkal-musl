/* The name `realpath' answers, from the environment where it answers it.
 *
 * musl's realpath resolves a name one component at a time, asking of each
 * whether it is a link (readlink, here kal_fs_link_read): `usr', `usr/lib',
 * `usr/lib/gcc', and so on, for every name. A language server resolves the
 * name of every header it reads; measured in one session of one, 872 000
 * enquiries where the program of the environment made 353 000 and 99 000
 * readlinks of /proc/self/fd, one for each file it opened. Only the
 * environment can answer for the whole name at once, and openkal 0.23 asks it
 * (kal_fs_resolved_name).
 *
 * musl/src/misc/realpath.c asks here first (musl/PATCHES.md). A name is
 * answered here when it has no `.' or `..' component and does not end in a
 * separator, so that resolving it lexically, as okm_resolve does, and
 * following its links agree; when its volume states KAL_FS_PROP_RESOLVED_NAME;
 * and when the runner beneath binds the name (it is referenced weakly, so that
 * an image of this library runs on a runner of openkal 0.22). Otherwise zero,
 * and musl's walk answers as before.
 */
#include "okm.h"
#include <openkal/fs.h>
extern __typeof(kal_fs_resolved_name) kal_fs_resolved_name __attribute__((__weak__));

#include <errno.h>
#include <fcntl.h>

/* No `.' or `..' component, and no separator at the end. */
static int plain(const char *name)
{
	const char *p = name;
	if (!*p) return 0;
	for (;;) {
		const char *s = p;
		while (*p && *p != '/' && *p != '\\') p++;
		const size_t n = (size_t)(p - s);
		if (n == 1 && s[0] == '.') return 0;
		if (n == 2 && s[0] == '.' && s[1] == '.') return 0;
		if (!*p) return 1;
		while (*p == '/' || *p == '\\') p++;
		if (!*p) return 0;
	}
}

long __okm_realpath(const char *name, char *out, size_t cap)
{
	if (!name || !plain(name)) return 0;
	if (okm_null_name(AT_FDCWD, name)) {
		static const char null_name[] = "/dev/null";
		if (cap < sizeof null_name) return -ENAMETOOLONG;
		for (size_t i = 0; i < sizeof null_name; i++) out[i] = null_name[i];
		return (long)(sizeof null_name - 1);
	}
	if (!kal_fs_resolved_name) return 0;
	struct okm_at at;
	if (okm_resolve(AT_FDCWD, name, &at, 0) != 0) return 0;
	if (!(kal_fs_props(at.base) & KAL_FS_PROP_RESOLVED_NAME)) return 0;
	const kal_intptr n = kal_fs_resolved_name(at.base, at.rel, at.len, out, cap);
	if (n == -(kal_intptr)kal_err_not_supported) return 0;
	if (n < 0) return -okm_errno((int)-n);
	if ((size_t)n >= cap) return -ENAMETOOLONG;
	out[n] = 0;
	/* A name whose root is a volume is spelled as this library spells the
	 * names it resolves, with forward separators (okm_fd.c, `join'). */
	if (n >= 3 && out[1] == ':' && (out[2] == '\\' || out[2] == '/'))
		for (kal_intptr i = 2; i < n; i++) if (out[i] == '\\') out[i] = '/';
	return (long)n;
}
