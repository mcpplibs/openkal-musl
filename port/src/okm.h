/* Declarations shared by the port layer.
 *
 * The port layer is where POSIX is reconstructed. openkal deliberately does
 * not have the shape of POSIX --- it has no global namespace of paths, no
 * descriptors, no duplication of the calling image --- and the specification
 * records that this is so that an implementation upon an environment which has
 * none of those things need not construct them. The consequence is that
 * something must construct them for a program that expects them, once, and
 * this is that thing.
 *
 * Two structures live here that clause 7.1 forbids an *implementation* to
 * have: a table of descriptors, and a resolver of names. Their being here
 * rather than there is the whole of the arrangement. An implementation that
 * contained them would contain one copy per environment; the port layer
 * contains one copy for every environment, and it is the copy this experiment
 * measures.
 */
#ifndef OKM_H
#define OKM_H

#include <openkal.h>
#include <stddef.h>

/* ⭐ WHICH OPENKAL INTERFACES THE IMPLEMENTATION BENEATH PROVIDES.
 *
 * openkal is composable. An implementation provides an interface in whole or
 * not at all (clause 6.1), and one that cannot provide `fs' does not provide
 * it, because clause 6.2 forbids the alternative in terms: an operation that is
 * present and always fails is a defect.
 *
 * ⚠️ THE CONSEQUENCE FOR A C LIBRARY, WHICH IS WHY THIS BLOCK EXISTS.
 *
 * A C library calls those interfaces. If it is BUILT calling one the
 * implementation beneath does not provide, the link fails --- and it fails for
 * every program, including one that never opens a file, because `fclose' is on
 * the exit path of anything that touches stdio and the descriptor table closes
 * what it holds.
 *
 * Measured 2026-08-23 on riscv64-none-elf, above an implementation providing
 * openkal's core set: a program whose entire text was `import std;' and one
 * line of output failed to link on fifteen names it can never reach.
 *
 * ⭐ SO THE C LIBRARY IS CONFIGURED AND AN OPENKAL IMPLEMENTATION IS NOT. THAT
 * LOOKS LIKE A CONTRADICTION AND IS NOT ONE.
 *
 *   An openkal implementation may not answer "unsupported", because openkal's
 *   surface has no such answer to give and a caller could not act on one.
 *
 *   A C library may, because POSIX's surface HAS one --- ENOSYS --- and every
 *   caller of `open' already handles a failure.
 *
 * Absence expressed as absence at one layer becomes absence expressed as a
 * defined error at the next. That is the two layers doing their jobs, and the
 * place where it would go wrong is a library that returned SUCCESS having done
 * nothing.
 *
 * ⭐ THREE MACROS HERE AND ALL THE CONSEQUENCES IN ONE OTHER FILE. This block
 * says what the environment HAS; okm_opt.h says what follows, and it is the
 * only file in the port that reads these. A source that calls an interface
 * which need not be there names `okm_fs_*' rather than `kal_fs_*', and never
 * asks the question itself --- because the same question asked in forty places
 * is answered wrongly in the forty-first.
 *
 * ⚠️ THE TARGET IS A PROXY FOR THE IMPLEMENTATION, AND AN IMPERFECT ONE.
 *
 * The manifest clears these for cfg(os="none"), because a target with no
 * operating system is the case where an implementation providing storage is
 * the exception. It is not the same statement --- a board CAN carry a real
 * filesystem --- so each is `#ifndef'-guarded and a project that has one turns
 * it back on from its own build. mcpp conditions on the target and not on which
 * package satisfies a capability; when it can, this block is where that would
 * be read instead, and nothing below would change. */
#ifndef OKM_HAS_FS
#  define OKM_HAS_FS 1
#endif
#ifndef OKM_HAS_PROCESS
#  define OKM_HAS_PROCESS 1
#endif
#ifndef OKM_HAS_TASK
#  define OKM_HAS_TASK 1
#endif

#define OKM_MAX_FD    1024
#define OKM_MAX_DESC  512
#define OKM_MAX_SOCK  128
#define OKM_MAX_PATH  4096
#define OKM_MAX_DIRS  64
#define OKM_DIR_PATH  512

/* The bounds are fixed and exhaustion is refused rather than grown around. A
 * program that opens more descriptors than the table holds is told so, which is
 * the answer every environment gives at some number; allocating the table
 * instead would place it on the allocator, and the allocator obtains its
 * memory through this table. */

enum okm_kind {
	OKM_FREE = 0,
	OKM_STREAM,     /* a borrowed stream: the program's own three */
	OKM_CHANNEL,    /* an OWNED stream, obtained from openkal.process */
	OKM_FILE,       /* an owned file obtained from openkal.fs     */
	OKM_DIR,        /* an owned directory                          */
	/* A socket. Its state lives in the table in okm_net.c rather than here,
	 * because a socket passes through four states that no other descriptor has
	 * and carries two endpoints that no other descriptor needs. `sock' below is
	 * the index. */
	OKM_SOCKET,
};

/* An open file description, in the POSIX sense: what a descriptor refers to,
 * and what two descriptors share after being duplicated. The position is the
 * environment's for a file and this layer's only for a directory, because
 * openkal enumerates a directory through an iterator rather than a position. */
struct okm_desc {
	int          refs;
	int          kind;
	int          flags;          /* the open(2) flags this description carries */
	kal_uintptr  stream;         /* OKM_STREAM, OKM_CHANNEL and OKM_FILE      */
	struct kal_file file;
	struct kal_dir  dir;
	kal_uintptr  iter;           /* directory enumeration, when begun          */
	int          iter_open;
	int          path_slot;      /* index into the directory-name pool, or -1  */
	/* One entry read from the iterator and not yet delivered. openkal has no
	 * operation that returns an entry to an iterator, so an entry that does
	 * not fit in the caller's buffer is held here rather than lost. */
	int          pending;
	int          pending_kind;
	char         pending_name[256];
	int          sock;           /* OKM_SOCKET: the slot in okm_net.c, else -1 */
	/* ⭐ ONE BYTE READ AHEAD, WHICH IS HOW A READINESS ENQUIRY IS ANSWERED.
	 *
	 * openkal has no operation that reports whether a transfer would proceed.
	 * `openkal.timeout' bounds the transfer itself, and clause 6.3 records
	 * readiness notification among the mechanisms considered and NOT adopted:
	 * an interface reporting readiness would oblige every implementation to
	 * maintain a set and a context of its own.
	 *
	 * So `poll' is answered by attempting the transfer under a bound and
	 * keeping what it produced. A byte that arrived is a byte the descriptor
	 * has, which is what POLLIN asserts; holding it here is what makes the
	 * assertion true for the read that follows rather than merely true at the
	 * moment it was made. okm_desc already holds a directory entry for the same
	 * reason and by the same means.
	 *
	 * ⚠️ ONE BYTE AND NOT A BUFFER. A short read is a result every caller of
	 * `read' already handles, and a larger read-ahead would turn this into a
	 * second layer of buffering underneath stdio's. */
	int           ahead;          /* a byte is held                            */
	int           ahead_eof;      /* the bounded read reported end of input    */
	unsigned char ahead_byte;
};

/* The lock. Every operation upon the table is short, and contention is between
 * execution contexts of one program rather than between programs, so a lock
 * that spins and yields is adequate; using openkal.task's suspension primitive
 * here would make the table depend upon an interface an implementation may not
 * provide, and a program with one execution context would then not run on it. */
void okm_lock(void);
void okm_unlock(void);

/* The table. */
int  okm_fd_alloc(int from);                     /* the lowest free descriptor */
void okm_fd_release(int fd);
struct okm_desc* okm_desc_of(int fd);
int  okm_fd_bind(int fd, int kind, kal_uintptr stream,
                 struct kal_file file, struct kal_dir dir, int flags);
int  okm_fd_dup(int fd, int newfd, int cloexec); /* newfd < 0: lowest free     */
int  okm_fd_cloexec(int fd, int on);
int  okm_fd_get_cloexec(int fd);
void okm_table_init(void);

/* The stream descriptor `fd' named when the program began, for fd in [0,3), and
 * zero otherwise. What a spawn needs in order to tell a descriptor that has been
 * redirected from one that has not; okm_fd.c records why the question is about
 * the beginning rather than about the present. */
kal_uintptr okm_std_stream(int fd);

/* Where a name resolves to. `base' is a directory openkal supplied or one
 * opened beneath it; `rel' is the remainder, in the form openkal accepts:
 * relative, without a leading separator and without a component that ascends. */
struct okm_at {
	struct kal_dir base;
	char           rel[OKM_MAX_PATH];
};

/* Resolves a name as openat(2) would. Returns 0, or a negated error value.
 * `dirfd' may be AT_FDCWD. An empty name is accepted only when `empty_ok'. */
int okm_resolve(int dirfd, const char* path, struct okm_at* at, int empty_ok);

/* Records the absolute name of a directory that has just been opened, so that
 * a name resolved against that descriptor can ascend out of it. A directory
 * whose name is not recorded --- the pool is bounded --- still serves every
 * name that does not ascend. */
void okm_dir_remember(int fd, const char* abs);

/* The absolute form of a name, for the callers that need it before the
 * directory it resolves against is known. Returns 0 or a negated error. */
int okm_absolute(int dirfd, const char* path, char* out, size_t cap);

/* The working directory, which POSIX has and openkal does not: openkal names
 * every operation relative to a directory the program holds, so the notion of
 * a current one is the program's and is kept here. */
extern struct kal_dir okm_cwd_dir;
const char* okm_cwd_path(void);

/* Changes the working directory to a name, or to a descriptor that names one.
 * Returns 0 or a negated error value. */
int okm_chdir(int dirfd, const char* path);

/* Translation between openkal's closed set of error values and the values a
 * program written for Linux expects. The mapping is a table; it does not
 * reconstruct a namespace. */
int okm_errno(int kal_error_value);

/* The other direction of the same kind: a program's open(2) flags said in
 * openkal's vocabulary. Two callers reach it, `open' and the file action a
 * spawn may carry, and the table is written once. */
kal_uintptr okm_open_flags(int open_flags);

/* --- sockets, in okm_net.c -------------------------------------------------
 *
 * ⚠️ BSD SEPARATES `socket' FROM `connect' AND `bind'; openkal DOES NOT.
 * `kal_net_connect' produces a connection and there is no unbound socket to
 * produce first. So a descriptor made by `socket' holds nothing but the three
 * numbers it was given, and the openkal operation happens later --- at
 * `connect', or at `listen' once `bind' has recorded where. That deferral is
 * the whole of the adaptation, and it lives in one file.
 *
 * Every one of these returns 0 or a count, or a negated errno value, which is
 * the convention the dispatcher in okm_syscall.c passes straight through. */
int  okm_sock_open   (int domain, int type, int protocol);
int  okm_sock_bind   (int fd, const void* addr, unsigned len);
int  okm_sock_listen (int fd, int backlog);
int  okm_sock_accept (int fd, void* addr, unsigned* len, int flags);
int  okm_sock_connect(int fd, const void* addr, unsigned len);
int  okm_sock_name   (int fd, void* addr, unsigned* len, int peer);
int  okm_sock_shutdown(int fd, int how);
long okm_sock_send   (int fd, const void* buf, unsigned long len, int flags,
                      const void* addr, unsigned alen);
long okm_sock_recv   (int fd, void* buf, unsigned long len, int flags,
                      void* addr, unsigned* alen);
int  okm_sock_setopt (int fd, int level, int opt, const void* val, unsigned len);
int  okm_sock_getopt (int fd, int level, int opt, void* val, unsigned* len);

/* Released with the description that held it. Called from okm_fd.c, which is
 * the one place a description's lifetime ends. */
void okm_sock_release(int slot);

/* Waits up to `ns' for a socket to have a connection to accept or a message to
 * receive, and keeps what arrived so that the operation which follows finds it.
 * 1 ready, 0 the bound expired, negative a negated errno value. */
int  okm_sock_wait_in(struct okm_desc* d, kal_u64 ns);

/* Which of three shapes a socket descriptor has, so that the readiness code can
 * ask without knowing the socket table. A connected socket is a stream and
 * takes the same path a pipe takes; a listener and a datagram endpoint are
 * neither, and each waits in its own way. */
#define OKM_SOCK_SHAPE_IDLE   0   /* nothing can arrive on it yet             */
#define OKM_SOCK_SHAPE_STREAM 1   /* connected: the read-ahead applies        */
#define OKM_SOCK_SHAPE_OWN    2   /* a listener or a datagram endpoint        */
int  okm_sock_shape(struct okm_desc* d);

/* --- readiness and bounded transfer, in okm_poll.c ------------------------- */

/* The read-ahead a readiness enquiry left, delivered to a caller of `read'.
 *
 *   > 0             the count placed in the buffer
 *   0               nothing was held; the caller performs its own transfer
 *   OKM_AHEAD_EOF   an end of input the enquiry already observed. The read is
 *                   complete with zero bytes, and the mark is cleared: a
 *                   terminal may deliver more after one, and a pipe reports
 *                   the same end again at once, so neither is lost by
 *                   forgetting it. */
#define OKM_AHEAD_EOF (-1L)
long okm_take_ahead(struct okm_desc* d, void* buf, unsigned long len);

/* A transfer bounded in time. `ns' of zero is openkal's spelling of "no bound"
 * and is not what a non-blocking descriptor wants; `OKM_NOW_NS' is the smallest
 * bound there is, and an environment with a coarse clock rounds it up to its
 * own granularity rather than refusing it. Both report -ENOSYS where the
 * environment beneath provides no `openkal.timeout'. */
#define OKM_NOW_NS ((kal_u64)1)
long okm_timed_read (kal_uintptr stream, void* buf, unsigned long len, kal_u64 ns);
long okm_timed_write(kal_uintptr stream, const void* buf, unsigned long len, kal_u64 ns);

/* Whether the environment beneath can bound an operation at all. `O_NONBLOCK'
 * is refused where it cannot, rather than accepted and ignored. */
int  okm_can_bound(void);

/* poll(2) over a set, and the whole of what `select' is expressed as. */
long okm_poll(void* fds, unsigned long n, int timeout_ms);

/* The preopened directories, read once. */
int okm_preopen_count(void);
int okm_preopen(int index, struct kal_dir* dir, const char** name, size_t* len);

/* The seam itself, for the port sources that call it directly rather than
 * through musl's inline wrappers. Its declaration lives with the seam, so that
 * the width of an argument is stated in one place. */
#include "syscall_arch.h"

/* Startup, in the order it happens. */
void __okm_init_env(void);

#endif
