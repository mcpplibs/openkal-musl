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
 * for the duplicate to perform: an arbitrary sequence of file actions. Four of
 * the five are expressible in what openkal offers and are translated; the fifth
 * is refused rather than ignored.
 *
 * --- what the started program's three streams are -------------------------
 *
 * ⚠️⚠️ THIS FILE USED TO PASS `{0, 0, 0}' AND LET THE FILE ACTIONS OVERWRITE IT,
 * WHICH LOST EVERY REDIRECTION A CALLER HAD ALREADY PERFORMED.
 *
 * openkal spells "the stream its parent has" as a handle of zero (process.h),
 * and an implementation reads that as the descriptor the CALLING IMAGE holds ---
 * not the one this library's table holds. `dup2' rebinds this table and issues
 * no system call, because there is none to issue: openkal has no operation that
 * replaces one of a running program's own streams. So after
 *
 *     dup2(fd, 1);  posix_spawn(…)         or        dup2(fd, 1);  execve(…)
 *
 * the started program wrote to the stream this program was STARTED with, and the
 * file the caller had redirected onto stayed empty. Reported as
 * openkal-linux#13; measured by a consumer, not here, because the probe in
 * examples/subprocess started programs and never redirected first.
 *
 * ⭐ THE ATOM WAS ALREADY PRESENT. `kal_fs_stream' is a required operation of
 * `openkal.fs' and this port has stored its result in every file description
 * since okm_syscall.c opened one. What was missing was the route from the table
 * to the spawn, which is the same shape as the defect that report began with.
 *
 * ⭐ AND ZERO IS STILL PASSED WHERE NOTHING WAS REDIRECTED, WHICH IS LOAD-BEARING.
 * `KAL_PROCESS_PROP_STREAM_PASSING' exists because an environment may be able to
 * start a program that inherits and unable to place a stream of the caller's
 * choosing. Seeding with zero for an untouched descriptor asks for that capability
 * only when a caller has actually asked for something, so a backend without it
 * still runs every program that does not redirect.
 */
#define _GNU_SOURCE
#include "okm.h"
#include "okm_opt.h"

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

/* --- what the started program's three streams are -------------------------- */

/* Places one stream, and answers with an error value where the interface has no
 * way to say what the caller asked for.
 *
 * `placed' records whether anything the environment must be ABLE to place has
 * been placed. A caller that duplicates descriptor 1 onto position 1 when
 * descriptor 1 has not moved has asked for inheritance, so that case is
 * normalised back to zero: asking a backend for a capability it does not need
 * would refuse a spawn that can be performed.
 *
 * ⚠️⚠️ AND ZERO IS BOTH A SENTINEL AND A VALID HANDLE, WHICH IS NOT THIS FILE'S
 * DOING AND IS THIS FILE'S PROBLEM.
 *
 * `kal_spawn_streams' spells inheritance as a handle of zero. `kal_stream' has
 * no reserved value, and openkal-linux answers `kal_stdin()' with zero because
 * its streams are the environment's own descriptors. The two are consistent for
 * position zero --- placing standard input at standard input and inheriting it
 * are the same act --- and inexpressible for the one case where they differ: a
 * caller that redirects its OUTPUT onto its own standard input, which reaches
 * here as the handle zero at position one.
 *
 * ⇒ Refused, rather than passed on. Passing it on would be read as inheritance
 * and would start the program writing to the stream the caller had just
 * redirected away from --- the silent wrong answer this whole change exists to
 * remove. A refusal is one the caller can act upon. Recorded in
 * musl/PATCHES.md, and reported upstream: the specification reserves a value in
 * one interface that another interface may hand out. */
static int place(struct kal_spawn_streams* s, int pos, kal_uintptr stream,
                 int* placed)
{
	if (stream == okm_std_stream(pos)) stream = 0;
	else if (stream == 0)              return ENOSYS;
	else                               *placed = 1;
	if      (pos == 0) s->in  = stream;
	else if (pos == 1) s->out = stream;
	else               s->err = stream;
	return 0;
}

/* The stream a descriptor carries, or a reason it carries none.
 *
 * ⚠️ THE ANSWER IS DECIDED BY THE KIND AND NOT BY THE VALUE, and that distinction
 * is the whole of it: a stream handle of zero is a perfectly ordinary handle on
 * an implementation whose streams are its own descriptors, so "the handle is
 * zero" cannot be read as "there is no stream". A directory has no stream
 * because it is a directory; an unconnected socket has none because openkal
 * produces a connection and a stream together, which is the one kind for which
 * zero does mean absence and okm_net.c is where that convention is set.
 *
 * The classification is the one okm_syscall.c's `stream_of' makes for reading
 * and writing, and the two agree deliberately: a descriptor a program can write
 * to is a descriptor a started program can be given. */
static int stream_for_spawn(int fd, kal_uintptr* out)
{
	struct okm_desc* d = okm_desc_of(fd);
	if (!d) return EBADF;
	switch (d->kind) {
	case OKM_STREAM:
	case OKM_CHANNEL:
	case OKM_FILE:
		*out = d->stream;
		return 0;
	case OKM_SOCKET:
		if (d->stream == 0) return ENOTCONN;
		*out = d->stream;
		return 0;
	default:
		return EBADF;          /* a directory, or a descriptor that is not open */
	}
}

/* The three positions before any file action is applied.
 *
 * ⭐ THIS IS THE WHOLE OF THE FIX FOR openkal-linux#13, AND IT IS THREE LINES OF
 * DECISION rather than a mechanism: a descriptor that still names what it named
 * when the program began is inheritance and is spelled zero; one that names
 * something else is a redirection the caller performed and is carried across;
 * one that names nothing is refused. */
static int seed(struct kal_spawn_streams* s, int* placed)
{
	for (int fd = 0; fd < 3; fd++) {
		kal_uintptr stream = 0;
		int e = stream_for_spawn(fd, &stream);
		if (!e) e = place(s, fd, stream, placed);
		if (e) return e;
	}
	return 0;
}

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
	int placed = 0;                 /* a stream of the caller's choosing */
	int refused = seed(&streams, &placed);

	/* Files a file action opened. They belong to this call rather than to the
	 * caller --- the caller asked for a program started upon them, not for a
	 * file --- so each is released once the spawn has been performed. At most
	 * three can exist, because there are three positions to place them in. */
	struct kal_file opened[3];
	int opened_n = 0;

	/* Resolving a file action's name needs one of these and it is four kilobytes.
	 * Static, under this file's lock, for the same reason the argument vectors
	 * above are: a second one on the stack would double the frame of a function
	 * that a program with a small thread stack reaches. */
	static struct okm_at fa_at;

	if (!refused && fa) {
		/* ⚠️⚠️ IN THE ORDER THEY WERE ADDED, WHICH IS NOT THE ORDER THE LIST IS
		 * IN. musl's `posix_spawn_file_actions_add*' PREPEND, so `__actions'
		 * names the most recent one; musl's own `posix_spawn' walks to the tail
		 * and then follows `prev'. This file followed `next' and therefore
		 * applied a caller's actions in reverse.
		 *
		 * Invisible until two actions name one position: `popen' emits a single
		 * `adddup2' and was correct throughout. A caller that opened a file onto
		 * position 1 and then duplicated something else onto it would have got
		 * the file. */
		struct fdop* op = fa->__actions;
		if (op) while (op->next) op = op->next;
		for (; op; op = op->prev) {
			switch (op->cmd) {
			case FDOP_DUP2: {
				if (op->fd > 2 || op->fd < 0) { refused = ENOSYS; break; }
				kal_uintptr s = 0;
				refused = stream_for_spawn(op->srcfd, &s);
				if (refused) break;
				refused = place(&streams, op->fd, s, &placed);
				break;
			}
			case FDOP_OPEN: {
				/* ⭐ EXPRESSIBLE, AND IT WAS REFUSED. `kal_fs_open' produces the
				 * file and `kal_fs_stream' produces the stream to place, both of
				 * them required operations this port already calls elsewhere.
				 * Refusing it forced every caller wanting a program's output in
				 * a file through `dup2' --- which is the route openkal-linux#13
				 * reported as not working. */
				if (op->fd > 2 || op->fd < 0) { refused = ENOSYS; break; }
				if (opened_n >= 3) { refused = ENOSYS; break; }
				const long r = okm_resolve(AT_FDCWD, op->path, &fa_at, 0);
				if (r) { refused = (int)-r; break; }
				struct kal_file f;
				/* ⚠️ `op->mode' is not among the inputs, exactly as it is not for
				 * `open'. README.md's divergence table records what a program
				 * observes; a mode accepted here and dropped beneath would be
				 * this file reporting success having done something else. */
				const int oe = okm_fs_open(fa_at.base, fa_at.rel, slen(fa_at.rel),
				                           okm_open_flags(op->oflag), &f);
				if (oe != kal_ok) { refused = okm_errno(oe); break; }
				opened[opened_n++] = f;
				refused = place(&streams, op->fd, okm_fs_stream(f), &placed);
				break;
			}
			case FDOP_CLOSE:
				/* ⭐ TWO ANSWERS, DECIDED BY WHICH DESCRIPTOR IS NAMED. The
				 * comment this replaces gave one of them for both, on the ground
				 * that "nothing is inherited that was not asked for" --- which is
				 * true above position two and false at or below it, because the
				 * three positions are inherited by construction.
				 *
				 * ⚠️ So closing one of the three is an action that cannot be
				 * performed: openkal has no value meaning "no stream", and the
				 * value that looks like one means the opposite. Accepting it and
				 * doing nothing would hand a program the standard input its
				 * caller had just taken away, which is the failure a caller
				 * cannot see and cannot act upon. */
				if (op->fd > 2) break;
				refused = ENOSYS;
				break;
			case FDOP_CHDIR:
			case FDOP_FCHDIR:
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

	/* ⭐ THE CAPABILITY IS REQUIRED ONLY WHERE IT IS USED.
	 *
	 * `KAL_PROCESS_PROP_STREAM_PASSING' exists because an environment may be
	 * able to start a program that inherits and unable to give it a stream of
	 * the caller's choosing. Every position that was left as inheritance is
	 * zero, so a program that redirects nothing asks for nothing, and only a
	 * caller that did redirect meets the refusal --- which is the answer it can
	 * act upon rather than a program started somewhere it did not ask for. */
	if (!refused && placed
	    && !(okm_process_props() & KAL_PROCESS_PROP_STREAM_PASSING))
		refused = ENOSYS;

	if (refused) {
		for (int i = 0; i < opened_n; i++) okm_fs_close_file(opened[i]);
		okm_unlock();
		return refused;
	}

	/* The vector is passed unaltered, argv[0] included. Clause 7.6: the
	 * started program reads its own name through kal_env_arg(0), so a caller
	 * that did not supply it could not predict what the program would read. */
	struct kal_process child;
	int e = okm_process_spawn(at.base, at.rel, slen(at.rel),
	                          a_ptr, a_len, (kal_uintptr)argc,
	                          e_ptr, e_len, (kal_uintptr)envc,
	                          &streams, &child);

#ifdef _WIN32
	/* The one place where this environment's naming of a program differs from
	 * the naming this interface presents.
	 *
	 * A program here is a file whose name ends in a particular suffix, and a
	 * caller of this interface names programs the way this interface's callers
	 * name them --- without one. Every C library for this environment resolves
	 * that difference, and it is resolved here rather than beneath, because
	 * openkal is deliberately literal about names: it passes on the name it was
	 * given and does not know that a program is a kind of file.
	 *
	 * It is tried second rather than first, so a file that genuinely bears the
	 * name is preferred to one that bears the name and the suffix. */
	if (e == kal_err_not_found) {
		const size_t n = slen(at.rel);
		int has_suffix = 0;
		for (size_t i = n; i > 0; i--) {
			if (at.rel[i - 1] == '/') break;
			if (at.rel[i - 1] == '.') { has_suffix = 1; break; }
		}
		if (!has_suffix && n + 4 < sizeof at.rel) {
			at.rel[n + 0] = '.'; at.rel[n + 1] = 'e';
			at.rel[n + 2] = 'x'; at.rel[n + 3] = 'e'; at.rel[n + 4] = 0;
			e = okm_process_spawn(at.base, at.rel, n + 4,
			                      a_ptr, a_len, (kal_uintptr)argc,
			                      e_ptr, e_len, (kal_uintptr)envc,
			                      &streams, &child);
		}
	}
#endif

	/* ⭐ THE FILES A FILE ACTION OPENED ARE RELEASED HERE, AND THE STARTED
	 * PROGRAM KEEPS ITS STREAM.
	 *
	 * That the second survives the first is what `kal_process_channel' already
	 * requires of an implementation in as many words --- "a parent that does not
	 * release `theirs' after the spawn never observes the end of input on
	 * `mine'" --- so a spawn that did not carry the stream across would make that
	 * instruction impossible to follow. The same holds for a stream that came
	 * from a file, and openkal-musl asks the specification to say so for streams
	 * in general rather than for one of them. */
	for (int i = 0; i < opened_n; i++) okm_fs_close_file(opened[i]);

	okm_unlock();
	if (e != kal_ok) return okm_errno(e);

	const int pid = __okm_child_record(child);
	if (pid < 0) { okm_process_close(child); return EAGAIN; }
	*res = (pid_t)pid;
	return 0;
}

weak_alias(__posix_spawn, posix_spawn);
