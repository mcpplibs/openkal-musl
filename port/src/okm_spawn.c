/* Starting another program.
 *
 * The second of musl's own sources this port replaces, and the reason is the
 * same shape as the first. musl starts a program by duplicating itself and
 * then replacing the duplicate: `posix_spawn' calls clone with CLONE_VFORK and
 * the duplicate calls execve. openkal has neither operation, and clause 7.1 of
 * the specification records why: duplicating an address space and its
 * execution state cannot be performed faithfully on every environment openkal
 * targets, so an implementation obliged to reproduce it would be building a
 * compatibility layer.
 *
 * What openkal has instead is the composite: kal_process_spawn starts a named
 * program with an argument vector, a set of named values and a choice of
 * streams. That is what `posix_spawn' means, so the replacement is a
 * translation of arguments rather than a reconstruction of a mechanism --- and
 * it is shorter than the code it replaces.
 *
 * What is lost is the part of `posix_spawn' that is expressed as instructions
 * for the duplicate to perform: an arbitrary sequence of file actions. Three
 * of the five are expressible in what openkal offers and are translated; the
 * other two are refused rather than ignored.
 */
#define _GNU_SOURCE
#include "okm.h"

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
/* musl's own description of a file action, included by path rather than
 * copied: a copy would be a second definition of a structure this file reads
 * and musl writes, and the two would agree only until one changed. */
#include "../../musl/src/process/fdop.h"
#undef malloc
#undef calloc
#undef realloc
#undef free

#define OKM_SPAWN_MAX 512

int __okm_child_record(struct kal_process h);

static size_t slen(const char* s) { size_t n = 0; while (s && s[n]) n++; return n; }

static int count(char* const* v) { int n = 0; while (v && v[n]) n++; return n; }

int __posix_spawn(pid_t* restrict res, const char* restrict path,
                  const posix_spawn_file_actions_t* fa,
                  const posix_spawnattr_t* restrict attr,
                  char* const argv[restrict], char* const envp[restrict])
{
	if (!res || !path) return EINVAL;
	if (attr && (attr->__flags & ~(POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK)))
		return ENOSYS;

	struct okm_at at;
	{
		const long r = okm_resolve(AT_FDCWD, path, &at, 0);
		if (r) return (int)-r;
	}

	const int argc = count(argv);
	const int envc = count(envp);
	if (argc >= OKM_SPAWN_MAX || envc >= OKM_SPAWN_MAX) return E2BIG;

	static const char* a_ptr[OKM_SPAWN_MAX];
	static kal_uintptr  a_len[OKM_SPAWN_MAX];
	static const char* e_ptr[OKM_SPAWN_MAX];
	static kal_uintptr  e_len[OKM_SPAWN_MAX];

	okm_lock();
	for (int i = 0; i < argc; i++) { a_ptr[i] = argv[i]; a_len[i] = slen(argv[i]); }
	for (int i = 0; i < envc; i++) { e_ptr[i] = envp[i]; e_len[i] = slen(envp[i]); }

	/* The streams the started program receives. Zero means it inherits the
	 * corresponding stream of its parent, which is what an environment with no
	 * general mechanism for passing handles can always provide. */
	struct kal_spawn_streams streams = { 0, 0, 0 };
	int refused = 0;
	if (fa) {
		for (struct fdop* op = fa->__actions; op; op = op->next) {
			switch (op->cmd) {
			case FDOP_DUP2: {
				if (op->fd > 2 || op->fd < 0) { refused = ENOSYS; break; }
				struct okm_desc* d = okm_desc_of(op->srcfd);
				if (!d) { refused = EBADF; break; }
				const kal_uintptr s = d->stream;
				if (op->fd == 0) streams.in = s;
				else if (op->fd == 1) streams.out = s;
				else streams.err = s;
				break;
			}
			case FDOP_CLOSE:
				/* Nothing is inherited that was not asked for, so closing a
				 * descriptor in the started program is already its state. */
				break;
			case FDOP_CHDIR:
			case FDOP_FCHDIR:
			case FDOP_OPEN:
			default:
				/* An action openkal cannot express. Performing the spawn
				 * without it would start the program in a state the caller did
				 * not ask for, which is worse than not starting it. */
				refused = ENOSYS;
				break;
			}
			if (refused) break;
		}
	}
	if (refused) { okm_unlock(); return refused; }

	/* The vector is passed unaltered, argv[0] included. Clause 7.6: the
	 * started program reads its own name through kal_env_arg(0), so a caller
	 * that did not supply it could not predict what the program would read. */
	struct kal_process child;
	const int e = kal_process_spawn(at.base, at.rel, slen(at.rel),
	                                a_ptr, a_len, (kal_uintptr)argc,
	                                e_ptr, e_len, (kal_uintptr)envc,
	                                &streams, &child);
	okm_unlock();
	if (e != kal_ok) return okm_errno(e);

	const int pid = __okm_child_record(child);
	if (pid < 0) { kal_process_close(child); return EAGAIN; }
	*res = (pid_t)pid;
	return 0;
}

weak_alias(__posix_spawn, posix_spawn);
