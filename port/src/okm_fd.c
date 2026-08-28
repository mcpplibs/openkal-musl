/* Descriptors and names.
 *
 * Two things POSIX has and openkal does not are built here, and each is built
 * once for every environment rather than once per environment.
 *
 * A **descriptor** is a small integer that names an open resource and that a
 * program may duplicate, inherit and renumber. openkal has handles instead,
 * which are opaque machine words with no ordering and no small dense range,
 * because a small dense range obliges an environment that has none to invent
 * one. The table below is that invention, made once.
 *
 * A **name** in POSIX is resolved against a single global root. openkal has no
 * global root: it supplies a set of directories and every operation is
 * relative to one of them. Resolving `/etc/passwd' therefore means choosing
 * the supplied directory whose name is the longest prefix of that path and
 * opening the remainder relative to it --- one rule, in one place, rather than
 * the same rule in every program.
 *
 * A consequence worth stating: a program confined to a subtree finds no
 * supplied directory for a name outside it, and therefore fails to resolve the
 * name rather than reaching outside. Confinement is a property of what the
 * environment supplied, not of the program's cooperation.
 */
#include "okm.h"
/* For kal_process_channel_close: a channel end is owned and is released here.
 *
 * ⚠️ WEAK, for the reason okm_syscall.c states beside the same pair: a backend
 * that provides no `openkal.process' provides neither, and a strong reference
 * would make an interface clause 6.1 permits a backend to decline into one every
 * program must have. A descriptor of this kind cannot exist without the
 * operation that made it, so the test below can never fail in a program that has
 * one. */
#include <openkal/process.h>
extern __typeof(kal_process_channel_close) kal_process_channel_close __attribute__((__weak__));
#include "okm_opt.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>

/* --- translation ---------------------------------------------------------- */

int okm_errno(int e)
{
	switch (e) {
	case kal_ok:                return 0;
	case kal_err_invalid:       return EINVAL;
	case kal_err_again:         return EAGAIN;
	case kal_err_io:            return EIO;
	case kal_err_no_memory:     return ENOMEM;
	case kal_err_no_space:      return ENOSPC;
	case kal_err_permission:    return EACCES;
	case kal_err_not_supported: return ENOSYS;
	case kal_err_closed:        return EPIPE;
	case kal_err_not_found:     return ENOENT;
	case kal_err_exists:        return EEXIST;
	case kal_err_not_empty:     return ENOTEMPTY;
	case kal_err_is_directory:  return EISDIR;
	case kal_err_not_directory: return ENOTDIR;
	default:                    return EIO;
	}
}

/* How a program's open(2) flags are said in openkal's vocabulary.
 *
 * ⭐ ONE DECISION IN ONE PLACE. Two callers reach it --- `open' itself and the
 * file action a spawn may carry --- and a second derivation of the same table
 * would agree with this one until one of them was extended.
 *
 * ⚠️ The mode a caller supplies is not among the inputs, and its absence is not
 * an oversight: `kal_fs_open' takes what a file is opened FOR and not who may
 * later reach it. README.md records what a program observes as a result. */
kal_uintptr okm_open_flags(int flags)
{
	kal_uintptr want;
	switch (flags & O_ACCMODE) {
	case O_RDONLY: want = KAL_OPEN_READ; break;
	case O_WRONLY: want = KAL_OPEN_WRITE; break;
	default:       want = KAL_OPEN_READ | KAL_OPEN_WRITE; break;
	}
	if (flags & O_CREAT)  want |= KAL_OPEN_CREATE;
	if (flags & O_EXCL)   want |= KAL_OPEN_EXCLUSIVE;
	if (flags & O_TRUNC)  want |= KAL_OPEN_TRUNCATE;
	if (flags & O_APPEND) want |= KAL_OPEN_APPEND;
	return want;
}

/* --- the lock ------------------------------------------------------------- */

static volatile int g_lock;

void okm_lock(void)
{
	while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE))
		okm_task_yield();
}

void okm_unlock(void) { __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE); }

/* --- the table ------------------------------------------------------------ */

static struct okm_desc g_desc[OKM_MAX_DESC];
static struct { int desc; int cloexec; } g_fd[OKM_MAX_FD];
static char g_dirpath[OKM_MAX_DIRS][OKM_DIR_PATH];
static int  g_dirpath_used[OKM_MAX_DIRS];

/* ⭐ THE THREE STREAMS THE PROGRAM WAS STARTED WITH, KEPT BECAUSE A LATER
 * QUESTION IS ABOUT HISTORY RATHER THAN ABOUT THE PRESENT.
 *
 * When this library starts another program it must say what that program's
 * three streams are, and openkal spells "the ones its parent has" as a handle
 * of zero (process.h). Whether zero is the right answer is not "what does
 * descriptor 1 name" but "does descriptor 1 STILL name what it named when this
 * program began" --- because `dup2' rebinds this table and cannot rebind the
 * environment's own descriptor 1, so the two answers stopped agreeing the
 * moment a program redirected anything.
 *
 * Asking `kal_stdout()' again at the point of the spawn would give the same
 * value and would be the same question asked of the present. Recording it here
 * states that the comparison is with the beginning. */
static kal_uintptr g_std_stream[3];

kal_uintptr okm_std_stream(int fd)
{
	if (fd < 0 || fd > 2) return 0;
	return g_std_stream[fd];
}

static int desc_alloc(void)
{
	for (int i = 0; i < OKM_MAX_DESC; i++)
		if (g_desc[i].refs == 0) { g_desc[i].refs = 1; return i; }
	return -1;
}

static int path_slot_alloc(void)
{
	for (int i = 0; i < OKM_MAX_DIRS; i++)
		if (!g_dirpath_used[i]) { g_dirpath_used[i] = 1; g_dirpath[i][0] = 0; return i; }
	return -1;
}

static void desc_release(int d)
{
	if (d < 0 || d >= OKM_MAX_DESC) return;
	if (--g_desc[d].refs > 0) return;
	struct okm_desc* p = &g_desc[d];
	if (p->kind == OKM_FILE) okm_fs_close_file(p->file);
	else if (p->kind == OKM_DIR) okm_fs_close_dir(p->dir);
	/* ⚠️ A CHANNEL END IS OWNED AND A STREAM IS NOT, WHICH IS WHY THEY ARE TWO
	 * KINDS. openkal draws the same division: the three standard streams are
	 * borrowed and have no release, while a channel end is obtained and must be
	 * given back.
	 *
	 * Closing nothing here is what a borrowed stream requires and what a channel
	 * end cannot tolerate. Measured before this kind existed: a program that
	 * wrote to a pipe, closed the write end and read again waited for ever,
	 * because the end it closed was still open. */
	else if (p->kind == OKM_CHANNEL && kal_process_channel_close) {
		struct kal_stream s; s.h = p->stream;
		kal_process_channel_close(s);
	}
	/* A socket's own release is the one openkal operation that depends on which
	 * of the four states it reached, so it is performed where those states are
	 * known rather than reproduced here. */
	else if (p->kind == OKM_SOCKET) okm_sock_release(p->sock);
	if (p->path_slot >= 0) g_dirpath_used[p->path_slot] = 0;
	p->kind = OKM_FREE; p->iter = 0; p->iter_open = 0; p->path_slot = -1;
	p->pending = 0; p->sock = -1; p->ahead = 0; p->ahead_eof = 0;
}

int okm_fd_alloc(int from)
{
	if (from < 0) from = 0;
	for (int i = from; i < OKM_MAX_FD; i++)
		if (g_fd[i].desc < 0) return i;
	return -EMFILE;
}

void okm_fd_release(int fd)
{
	if (fd < 0 || fd >= OKM_MAX_FD) return;
	const int d = g_fd[fd].desc;
	g_fd[fd].desc = -1; g_fd[fd].cloexec = 0;
	desc_release(d);
}

struct okm_desc* okm_desc_of(int fd)
{
	if (fd < 0 || fd >= OKM_MAX_FD) return 0;
	const int d = g_fd[fd].desc;
	if (d < 0) return 0;
	return &g_desc[d];
}

int okm_fd_bind(int fd, int kind, kal_uintptr stream,
                struct kal_file file, struct kal_dir dir, int flags)
{
	if (fd < 0 || fd >= OKM_MAX_FD) return -EBADF;
	const int d = desc_alloc();
	if (d < 0) return -ENFILE;
	struct okm_desc* p = &g_desc[d];
	p->kind = kind; p->flags = flags; p->stream = stream;
	p->file = file; p->dir = dir; p->iter = 0; p->iter_open = 0;
	p->path_slot = -1; p->pending = 0;
	p->sock = -1; p->ahead = 0; p->ahead_eof = 0;
	if (g_fd[fd].desc >= 0) desc_release(g_fd[fd].desc);
	g_fd[fd].desc = d;
	g_fd[fd].cloexec = (flags & O_CLOEXEC) ? 1 : 0;
	return fd;
}

int okm_fd_dup(int fd, int newfd, int cloexec)
{
	if (fd < 0 || fd >= OKM_MAX_FD || g_fd[fd].desc < 0) return -EBADF;
	if (newfd < 0) {
		newfd = okm_fd_alloc(0);
		if (newfd < 0) return newfd;
	}
	if (newfd >= OKM_MAX_FD) return -EBADF;
	if (newfd == fd) return newfd;
	if (g_fd[newfd].desc >= 0) desc_release(g_fd[newfd].desc);
	g_desc[g_fd[fd].desc].refs++;
	g_fd[newfd].desc = g_fd[fd].desc;
	g_fd[newfd].cloexec = cloexec;
	return newfd;
}

int okm_fd_cloexec(int fd, int on)
{
	if (fd < 0 || fd >= OKM_MAX_FD || g_fd[fd].desc < 0) return -EBADF;
	g_fd[fd].cloexec = on ? 1 : 0;
	return 0;
}

int okm_fd_get_cloexec(int fd)
{
	if (fd < 0 || fd >= OKM_MAX_FD || g_fd[fd].desc < 0) return -EBADF;
	return g_fd[fd].cloexec;
}

/* --- the supplied directories --------------------------------------------- */

/* The names are held here rather than pointed at. openkal 0.9 copies a name
 * into the caller's buffer instead of answering with a pointer into its own,
 * which is what lets this library behave the same whether the implementation is
 * linked into it, loaded beside it, or across a boundary. */
static struct { struct kal_dir dir; char name[256]; size_t len; } g_pre[16];
static int g_npre;

struct kal_dir okm_cwd_dir;
static char    g_cwd[OKM_MAX_PATH] = "/";

const char* okm_cwd_path(void) { return g_cwd; }

int okm_preopen_count(void) { return g_npre; }

int okm_preopen(int i, struct kal_dir* dir, const char** name, size_t* len)
{
	if (i < 0 || i >= g_npre) return -EINVAL;
	if (dir)  *dir  = g_pre[i].dir;
	if (name) *name = g_pre[i].name;
	if (len)  *len  = g_pre[i].len;
	return 0;
}

static size_t copy_str(char* dst, const char* src, size_t n, size_t cap)
{
	if (n >= cap) n = cap ? cap - 1 : 0;
	for (size_t i = 0; i < n; i++) dst[i] = src[i];
	dst[n] = 0;
	return n;
}

void okm_table_init(void)
{
	for (int i = 0; i < OKM_MAX_FD; i++) { g_fd[i].desc = -1; g_fd[i].cloexec = 0; }
	for (int i = 0; i < OKM_MAX_DESC; i++) { g_desc[i].path_slot = -1; g_desc[i].sock = -1; }

	/* The program's own three streams are descriptors 0, 1 and 2, which is
	 * what every program above this library assumes without saying so. They
	 * are borrowed: openkal does not release them and neither does this. */
	struct kal_file nof = { 0 };
	struct kal_dir  nod = { 0 };
	g_std_stream[0] = kal_stdin().h;
	g_std_stream[1] = kal_stdout().h;
	g_std_stream[2] = kal_stderr().h;
	okm_fd_bind(0, OKM_STREAM, g_std_stream[0], nof, nod, O_RDONLY);
	okm_fd_bind(1, OKM_STREAM, g_std_stream[1], nof, nod, O_WRONLY);
	okm_fd_bind(2, OKM_STREAM, g_std_stream[2], nof, nod, O_WRONLY);

	const kal_uintptr n = okm_fs_preopen_count();
	g_npre = 0;
	for (kal_uintptr i = 0; i < n && g_npre < 16; i++) {
		struct kal_dir d; kal_uintptr l = 0;
		/* The name is copied into this layer's own storage. It answered with a
		 * pointer into the implementation's, which is meaningful only while the
		 * implementation shares this address space --- and this library is the
		 * one consumer that must behave the same whichever way it is reached. */
		if (okm_fs_preopen(i, &d, g_pre[g_npre].name,
		                   sizeof g_pre[g_npre].name - 1, &l) != kal_ok) continue;
		if (l >= sizeof g_pre[g_npre].name) continue;
		g_pre[g_npre].name[l] = 0;
		g_pre[g_npre].dir = d; g_pre[g_npre].len = l;
		g_npre++;
	}

	/* The first supplied directory is the one the program was started in, and
	 * its name is what the program will report when asked where it is. An
	 * implementation whose environment has no global path namespace supplies a
	 * name that is not a path, and getcwd then reports that name --- which is
	 * the honest answer, since there is no other. */
	if (g_npre > 0) {
		okm_cwd_dir = g_pre[0].dir;
		copy_str(g_cwd, g_pre[0].name, g_pre[0].len, sizeof g_cwd);
	}
}

/* --- resolving a name ----------------------------------------------------- */

/* Removes `.', empty components and `..' from a path, in place of the
 * environment doing so. openkal refuses a name that ascends, so a name that
 * ascends is resolved here --- lexically, which is what a program means by it
 * in every case except one that passes through a symbolic link. That exception
 * is recorded in the README rather than hidden. */
static int normalise(const char* in, char* out, size_t cap)
{
	size_t o = 0;
	out[0] = 0;
	for (const char* p = in; *p; ) {
		while (*p == '/') p++;
		if (!*p) break;
		const char* s = p;
		while (*p && *p != '/') p++;
		const size_t n = (size_t)(p - s);
		if (n == 1 && s[0] == '.') continue;
		if (n == 2 && s[0] == '.' && s[1] == '.') {
			while (o > 0 && out[o - 1] != '/') o--;
			if (o > 0) o--;            /* drop the separator too */
			continue;
		}
		if (o + n + 2 > cap) return -ENAMETOOLONG;
		if (o) out[o++] = '/';
		for (size_t i = 0; i < n; i++) out[o++] = s[i];
	}
	out[o] = 0;
	return 0;
}

/* Where an absolute name begins.
 *
 * openkal does not say how an environment spells a global name, only that a
 * supplied directory has one and that the names are distinct. Two spellings
 * occur among the environments this library is built for: one writes a
 * separator first, and one writes a volume first. Both are recognised here and
 * neither is assumed --- a resolver that knew which would be a resolver for one
 * of them.
 *
 * The length returned is the part that is not a component and must survive
 * normalisation. Zero means the name is relative.
 */
static size_t root_length(const char* name)
{
	if (!name || !*name) return 0;
	if (name[0] == '/') return 1;
	if (name[1] == ':' && (name[2] == '/' || name[2] == '\\')
	    && ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= 'a' && name[0] <= 'z')))
		return 3;
	return 0;
}

static int is_prefix_at_boundary(const char* name, size_t len, const char* abs)
{
	if (len == 0) return 0;
	for (size_t i = 0; i < len; i++) if (abs[i] != name[i]) return 0;
	/* A supplied directory whose own name ends in a separator --- a root, on
	 * either spelling --- matches without requiring another. */
	if (name[len - 1] == '/') return 1;
	return abs[len] == 0 || abs[len] == '/';
}

/* Builds an absolute name from a base and a remainder, keeping whatever root
 * the one that has a root had. */
static int join(const char* prefix, const char* rel, char* out, size_t cap)
{
	char tmp[OKM_MAX_PATH];
	size_t o = 0;
	const size_t rroot = root_length(rel);
	const size_t proot = rroot ? 0 : root_length(prefix);
	const char*  base  = rroot ? "" : prefix + proot;

	for (const char* p = base; *p && o + 1 < sizeof tmp; p++) tmp[o++] = *p;
	if (o && tmp[o - 1] != '/') { if (o + 1 < sizeof tmp) tmp[o++] = '/'; }
	for (const char* p = rel + rroot; *p && o + 1 < sizeof tmp; p++) tmp[o++] = *p;
	if (o + 1 >= sizeof tmp) return -ENAMETOOLONG;
	tmp[o] = 0;

	const size_t keep = rroot ? rroot : proot;
	const char*  from = rroot ? rel : prefix;
	if (keep + 1 >= cap) return -ENAMETOOLONG;
	for (size_t i = 0; i < keep; i++) out[i] = from[i] == '\\' ? '/' : from[i];
	return normalise(tmp, out + keep, cap - keep);
}

int okm_resolve(int dirfd, const char* path, struct okm_at* at, int empty_ok)
{
	if (!path) return -EFAULT;
	if (!*path && !empty_ok) return -ENOENT;

	char abs[OKM_MAX_PATH];
	int have_abs = 0;

	if (root_length(path)) {
		const int r = join("", path, abs, sizeof abs);
		if (r) return r;
		have_abs = 1;
	} else if (dirfd == AT_FDCWD) {
		const int r = join(g_cwd, path, abs, sizeof abs);
		if (r) return r;
		have_abs = 1;
	} else {
		struct okm_desc* d = okm_desc_of(dirfd);
		if (!d || d->kind != OKM_DIR) return -ENOTDIR;
		if (d->path_slot >= 0 && g_dirpath[d->path_slot][0]) {
			const int r = join(g_dirpath[d->path_slot], path, abs, sizeof abs);
			if (r) return r;
			have_abs = 1;
		} else {
			/* A directory whose own name is not known: the remainder is used
			 * as it stands, which is correct for every name that does not
			 * ascend and is refused for one that does. */
			char norm[OKM_MAX_PATH];
			const int r = normalise(path, norm, sizeof norm);
			if (r) return r;
			for (const char* p = norm; *p; ) {
				if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == 0)) return -EACCES;
				while (*p && *p != '/') p++;
				while (*p == '/') p++;
			}
			at->base = d->dir;
			copy_str(at->rel, norm, __builtin_strlen(norm), sizeof at->rel);
			if (!at->rel[0]) copy_str(at->rel, ".", 1, sizeof at->rel);
			return 0;
		}
	}

	if (!have_abs) return -EINVAL;

	/* The supplied directory whose name is the longest prefix of the path.
	 * Longest rather than first: a program given both the whole file system
	 * and one directory beneath it should reach the second through the
	 * handle that names it, so that a confinement expressed by supplying
	 * fewer directories is the only thing that changes. */
	int best = -1; size_t best_len = 0;
	for (int i = 0; i < g_npre; i++) {
		if (!is_prefix_at_boundary(g_pre[i].name, g_pre[i].len, abs)) continue;
		if (best < 0 || g_pre[i].len > best_len) { best = i; best_len = g_pre[i].len; }
	}
	if (best < 0) return -ENOENT;

	const char* rest = abs + best_len;
	while (*rest == '/') rest++;
	at->base = g_pre[best].dir;
	if (!*rest) copy_str(at->rel, ".", 1, sizeof at->rel);
	else        copy_str(at->rel, rest, __builtin_strlen(rest), sizeof at->rel);
	return 0;
}

/* Records the absolute name of a directory that has just been opened, so that
 * a later name resolved against it can ascend. */
void okm_dir_remember(int fd, const char* abs)
{
	struct okm_desc* d = okm_desc_of(fd);
	if (!d || d->kind != OKM_DIR) return;
	if (d->path_slot < 0) d->path_slot = path_slot_alloc();
	if (d->path_slot < 0) return;
	copy_str(g_dirpath[d->path_slot], abs, __builtin_strlen(abs), OKM_DIR_PATH);
}

int okm_chdir(int dirfd, const char* path)
{
	struct kal_dir target;
	char abs[OKM_MAX_PATH];

	if (path) {
		struct okm_at at;
		const int r = okm_resolve(dirfd, path, &at, 0);
		if (r) return r;
		const int e = okm_fs_open_dir(at.base, at.rel, __builtin_strlen(at.rel), &target);
		if (e != kal_ok) return -okm_errno(e);
		if (root_length(path)) { if (join("", path, abs, sizeof abs)) return -ENAMETOOLONG; }
		else                   { if (join(g_cwd, path, abs, sizeof abs)) return -ENAMETOOLONG; }
	} else {
		struct okm_desc* d = okm_desc_of(dirfd);
		if (!d || d->kind != OKM_DIR) return -ENOTDIR;
		target = d->dir;
		if (d->path_slot >= 0 && g_dirpath[d->path_slot][0])
			copy_str(abs, g_dirpath[d->path_slot], __builtin_strlen(g_dirpath[d->path_slot]), sizeof abs);
		else
			copy_str(abs, g_cwd, __builtin_strlen(g_cwd), sizeof abs);
		/* The directory is now referred to twice; the description keeps its
		 * own handle and the working directory takes another by opening it
		 * through itself. */
		struct kal_dir own;
		if (okm_fs_open_dir(d->dir, ".", 1, &own) == kal_ok) target = own;
	}

	okm_cwd_dir = target;
	copy_str(g_cwd, abs, __builtin_strlen(abs), sizeof g_cwd);
	if (!g_cwd[0]) copy_str(g_cwd, "/", 1, sizeof g_cwd);
	return 0;
}

int okm_absolute(int dirfd, const char* path, char* out, size_t cap)
{
	if (!path) return -EFAULT;
	if (root_length(path)) return join("", path, out, cap);
	if (dirfd == AT_FDCWD) return join(g_cwd, path, out, cap);
	struct okm_desc* d = okm_desc_of(dirfd);
	if (!d || d->kind != OKM_DIR) return -ENOTDIR;
	if (d->path_slot < 0 || !g_dirpath[d->path_slot][0]) return -ENOENT;
	return join(g_dirpath[d->path_slot], path, out, cap);
}
