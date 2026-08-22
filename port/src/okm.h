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
	OKM_FILE,       /* an owned file obtained from openkal.fs     */
	OKM_DIR,        /* an owned directory                          */
};

/* An open file description, in the POSIX sense: what a descriptor refers to,
 * and what two descriptors share after being duplicated. The position is the
 * environment's for a file and this layer's only for a directory, because
 * openkal enumerates a directory through an iterator rather than a position. */
struct okm_desc {
	int          refs;
	int          kind;
	int          flags;          /* the open(2) flags this description carries */
	kal_uintptr  stream;         /* valid for OKM_STREAM and OKM_FILE          */
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
