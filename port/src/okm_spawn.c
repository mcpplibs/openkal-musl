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
#include <limits.h>
#include <spawn.h>
#include <stddef.h>
#include <stdlib.h>
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
/* How many files one set of file actions may open. Eight rather than three: a
 * caller may name one position more than once, and every file it opened has to
 * be released whether or not its stream ended up being the one placed. */
#define OKM_SPAWN_MAX_OPEN 8

int __okm_child_record(struct kal_process h);

static size_t slen(const char* s) { size_t n = 0; while (s && s[n]) n++; return n; }

static int count(char* const* v) { int n = 0; while (v && v[n]) n++; return n; }

/* --- can this name be started at all? --------------------------------------
 *
 * ⚠️⚠️ ASKED HERE BECAUSE THE ANSWER DOES NOT COME BACK FROM BENEATH, AND A
 * CALLER OF `execvp' CANNOT PROCEED WITHOUT IT.
 *
 * An implementation starts a program by duplicating itself and replacing the
 * duplicate, and the replacement happens in the DUPLICATE --- so a name that
 * cannot be started is discovered by a program that is no longer this one.
 * openkal-linux ends that duplicate with 127 (`process.cpp'), and its
 * `kal_process_spawn' returns `kal_ok' with a handle: the failure is reported
 * to nobody.
 *
 * ⇒ `execve' then waited for the duplicate, read 127, and ENDED THE CALLING
 * PROGRAM with it. musl's `execvp' issues one `execve' per PATH entry and
 * relies on it RETURNING with errno set so it can try the next one, so the
 * search could not survive its first miss: a program named without a slash was
 * found only when it happened to sit in the first entry. Reported as
 * openkal-linux#13 and measured by a consumer, who also measured that
 * `bwrap' --- present at /usr/bin/bwrap --- was reported as not installed.
 *
 * ⭐ THE ENQUIRY IS ALREADY REQUIRED OF EVERY IMPLEMENTATION. `kal_fs_info' is
 * an operation of `openkal.fs' and this file already resolves the name through
 * `okm_resolve'; asking what the name refers to is one more call on a path that
 * is about to start a program anyway.
 *
 * ⚠️ AND IT ANSWERS TWO OF THE THREE QUESTIONS, WHICH IS WHY A3 IS STILL OPEN.
 * `ENOENT' and `ENOTDIR' are what a PATH search needs and are what this
 * settles. Whether an existing file may be EXECUTED is not something openkal
 * reports --- `kal_node_info' carries `writable' and no other permission --- so
 * a name that exists and cannot be run still ends the caller with 127. That
 * residue is recorded in README.md beside `access(X_OK)', which cannot answer
 * it either and for the same reason. The complete answer needs the backend to
 * report its own failure; asked for at openkal-linux. */
static int startable(struct kal_dir base, const char* rel)
{
	struct kal_node_info info = { .self_size = sizeof info };
	/* Resolves, because starting resolves. */
	const int e = okm_fs_info(base, rel, slen(rel), 0, KAL_INFO_KIND, &info);
	/* ⚠️ AN ENQUIRY THAT CANNOT BE MADE IS NOT AN ANSWER OF `NO'. A build
	 * configured without `openkal.fs' --- OKM_HAS_FS=0, which a machine with no
	 * storage is built with --- answers `not supported' here, and turning that
	 * into a refusal would stop a spawn this port would otherwise have
	 * attempted. The enquiry is an improvement on the failure that follows, not
	 * a precondition of it: where it cannot be made, the spawn answers as it
	 * did before this check existed. */
	if (e == kal_err_not_supported) return 0;
	if (e != kal_ok) return okm_errno(e);
	if (info.kind == kal_node_absent) return ENOENT;
	/* POSIX names this one: a directory is not a program, and `execve' upon one
	 * is EACCES rather than ENOENT. `execvp' continues its search on both. */
	if (info.kind == kal_node_directory) return EACCES;
	return 0;
}

/* The name to start, and whether it can be. On the one environment that spells
 * a program with a suffix the name may be rewritten here, so that the enquiry
 * and the spawn agree about which name they are talking about. */
static int startable_name(struct okm_at* at)
{
	const int e = startable(at->base, at->rel);
#ifdef _WIN32
	/* Tried second rather than first, so a file that genuinely bears the name
	 * is preferred to one that bears the name and the suffix --- the rule the
	 * spawn below already followed, moved up so that the enquiry follows it
	 * too. Without this the pre-check would refuse every name this environment
	 * would have found. */
	if (e == ENOENT) {
		const size_t n = slen(at->rel);
		int has_suffix = 0;
		for (size_t i = n; i > 0; i--) {
			if (at->rel[i - 1] == '/') break;
			if (at->rel[i - 1] == '.') { has_suffix = 1; break; }
		}
		if (!has_suffix && n + 4 < sizeof at->rel) {
			at->rel[n + 0] = '.'; at->rel[n + 1] = 'e';
			at->rel[n + 2] = 'x'; at->rel[n + 3] = 'e'; at->rel[n + 4] = 0;
			if (startable(at->base, at->rel) == 0) return 0;
			at->rel[n] = 0;                       /* put the name back */
		}
	}
#endif
	return e;
}

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
	if      (pos == 0) s->in.h  = stream;
	else if (pos == 1) s->out.h = stream;
	else               s->err.h = stream;
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
	/* ⚠️ musl carries the PATH SEARCH in this field: `posix_spawnp' stores
	 * `__execvpe' there and its `posix_spawn' calls it in the duplicate instead
	 * of `execve'. This file replaces that duplicate, so the field was read by
	 * nobody --- and `posix_spawnp("sh", …)' therefore started `./sh', failed,
	 * and REPORTED SUCCESS. The search is performed by `__posix_spawnp' below
	 * and never arrives here, so a function found in this field now is one this
	 * file does not know how to honour, and is refused rather than ignored. */
	if (attr && attr->__fn) return ENOSYS;

	struct okm_at at;
	{
		const long r = okm_resolve(AT_FDCWD, path, &at, 0);
		if (r) return (int)-r;
	}

	/* Before anything is locked or opened, because a name that cannot be
	 * started needs no cleanup and a caller searching a PATH needs the answer
	 * more than it needs anything else this function does. */
	{
		const int e = startable_name(&at);
		if (e) return e;
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
	struct kal_spawn_streams streams = { { 0 }, { 0 }, { 0 } };
	int placed = 0;                 /* a stream of the caller's choosing */
	int refused = seed(&streams, &placed);

	/* Files a file action opened. They belong to this call rather than to the
	 * caller --- the caller asked for a program started upon them, not for a
	 * file --- so each is released once the spawn has been performed.
	 *
	 * ⚠️ THE BOUND IS NOT THREE. Three positions can be placed, and a caller may
	 * name one of them more than once; POSIX says the last such action decides,
	 * and the earlier file is still open and still has to be released. The bound
	 * is stated rather than derived, and a sequence that exceeds it is refused
	 * rather than silently truncated. */
	struct kal_file opened[OKM_SPAWN_MAX_OPEN];
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
				if (opened_n >= OKM_SPAWN_MAX_OPEN) { refused = ENOSYS; break; }
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
				refused = place(&streams, op->fd, okm_fs_stream(f).h, &placed);
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

	/* ⭐ THE ONE ENVIRONMENT THAT SPELLS A PROGRAM WITH A SUFFIX IS ANSWERED
	 * BEFORE THIS POINT AND NOT AFTER IT.
	 *
	 * A program there is a file whose name ends in a particular suffix, and a
	 * caller of this interface names programs without one; the difference is
	 * resolved here rather than beneath, because openkal is deliberately
	 * literal about names and does not know that a program is a kind of file.
	 *
	 * ⚠️ It used to be resolved by RETRYING the spawn on `kal_err_not_found'.
	 * That cannot stay: the enquiry added above refuses an absent name before
	 * the spawn is reached, so the retry would never run and every suffixless
	 * name on that environment would be refused. `startable_name' therefore
	 * owns the choice, keeps the same order --- the bare name first --- and
	 * rewrites `at.rel' so that what was asked about is what is started. */

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

/* --- starting a program named without a path -------------------------------
 *
 * The third of musl's own sources this port replaces, and it is replaced
 * because the second one was.
 *
 * musl does not search a PATH here. It stores `__execvpe' in the attributes and
 * lets its `posix_spawn' call that IN THE DUPLICATE instead of `execve', so the
 * search happens inside a program that has already been started. This port has
 * no duplicate to run it in --- `__posix_spawn' translates a spawn into
 * `kal_process_spawn' --- so the field was read by nobody and the name was
 * taken as a path relative to the working directory. `posix_spawnp("sh", …)'
 * started `./sh', which is not there, and reported SUCCESS.
 *
 * ⇒ The search is performed here, one `__posix_spawn' per entry, which is the
 * arrangement musl's own `__execvpe' uses one `execve' per entry for. It works
 * for the same reason that one now works: `__posix_spawn' reports a name it
 * cannot start rather than starting something that ends with 127.
 *
 * The rules are musl's, so that a program moved onto this port meets the answer
 * it met before: a name containing a separator is not searched for; an empty
 * entry means the working directory; `EACCES' anywhere is remembered and
 * reported only if nothing is found; and any other error ends the search at
 * once, because it is not evidence about the next entry. */
int __posix_spawnp(pid_t* restrict res, const char* restrict file,
                   const posix_spawn_file_actions_t* fa,
                   const posix_spawnattr_t* restrict attr,
                   char* const argv[restrict], char* const envp[restrict])
{
	if (!res || !file) return EINVAL;
	if (!*file) return ENOENT;

	for (const char* s = file; *s; s++)
		if (*s == '/') return __posix_spawn(res, file, fa, attr, argv, envp);

	const char* path = getenv("PATH");
	/* musl's own default, and it is here for the same reason it is there: a
	 * program that never set PATH still has somewhere to be looked for. */
	if (!path) path = "/usr/local/bin:/bin:/usr/bin";

	const size_t k = slen(file);
	if (k > NAME_MAX) return ENAMETOOLONG;

	/* ⚠️ ON THE STACK, WHICH IS WHAT musl DOES TOO (a variable-length array of
	 * the same bound). The static buffers above are under this file's lock and
	 * this function runs before it is taken --- a static here would be a race
	 * between two contexts searching at once, which is worse than a frame. */
	char cand[OKM_MAX_PATH];
	int seen_eacces = 0;

	for (const char* p = path; ; ) {
		const char* z = p;
		while (*z && *z != ':') z++;
		const size_t n = (size_t)(z - p);
		/* An entry that cannot be joined to the name is skipped rather than
		 * truncated: a truncated name is a different name and might exist. */
		if (n + (n ? 1 : 0) + k + 1 <= sizeof cand) {
			size_t o = 0;
			for (size_t i = 0; i < n; i++) cand[o++] = p[i];
			if (o) cand[o++] = '/';
			for (size_t i = 0; i <= k; i++) cand[o + i] = file[i];

			const int e = __posix_spawn(res, cand, fa, attr, argv, envp);
			if (!e) return 0;
			if (e == EACCES) seen_eacces = 1;
			else if (e != ENOENT && e != ENOTDIR) return e;
		}
		if (!*z) break;
		p = z + 1;
	}
	return seen_eacces ? EACCES : ENOENT;
}

weak_alias(__posix_spawnp, posix_spawnp);
