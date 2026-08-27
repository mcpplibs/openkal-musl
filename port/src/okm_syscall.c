/* The Linux system-call interface, expressed over openkal.
 *
 * musl issues system calls and openkal offers operations, and this file is the
 * whole of the correspondence. It is one file because musl reaches its kernel
 * through one seam; the seam is `syscall_arch.h', replaced in port/include.
 *
 * Three kinds of entry appear below and they are deliberately distinguishable.
 *
 *   1. A call that corresponds to an openkal operation. Most do.
 *   2. A call that names something openkal does not have and that a program
 *      can be told it does not have. Those return -ENOSYS, and the C library
 *      above reports the failure to the program, which is the outcome the
 *      program's author already handles.
 *   3. A call that names something openkal does not have and that no program
 *      asks about. Signal masking with no signals to mask is the example;
 *      succeeding is correct rather than convenient.
 *
 * What does not appear is a fourth kind: a call that reports success while
 * doing nothing the caller depended upon. Clause 3.1 of the specification
 * calls that a simulation, and every place where the temptation arises ---
 * memory protection, signal handlers, ownership --- is answered with -ENOSYS
 * and recorded in the README.
 */
#define _GNU_SOURCE
#include "okm.h"
#include "okm_opt.h"
#include <openkal/random.h>
/* Weak, for the reason given at SYS_getrandom below: the interface is
 * optional, and an implementation that does not provide it is absent as a
 * definition rather than present and refusing. */
extern __typeof(kal_random_fill) kal_random_fill __attribute__((weak));

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <time.h>
#include <dirent.h>
#include <poll.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <spawn.h>
#include "kstat.h"

/* Starting a program: what SYS_execve is expressed as here. */
int __posix_spawn(pid_t* restrict, const char* restrict,
                  const posix_spawn_file_actions_t*, const posix_spawnattr_t* restrict,
                  char* const[restrict], char* const[restrict]);

#define OKM_PAGE 4096

static size_t slen(const char* s) { size_t n = 0; while (s && s[n]) n++; return n; }

/* --- reading and writing --------------------------------------------------- */

static syscall_arg_t stream_of(int fd, kal_uintptr* out)
{
	struct okm_desc* d = okm_desc_of(fd);
	if (!d) return -EBADF;
	if (d->kind == OKM_STREAM || d->kind == OKM_FILE) { *out = d->stream; return 0; }
	return -EISDIR;
}

static syscall_arg_t do_write(int fd, const void* buf, size_t len)
{
	kal_uintptr s;
	const syscall_arg_t r = stream_of(fd, &s);
	if (r) return r;
	if (len == 0) return 0;
	struct kal_stream st; st.h = s;
	const struct kal_io_result io = kal_stream_write(st, buf, len);
	/* openkal transfers the whole buffer or reports what prevented it, so a
	 * short write cannot be reported as success here and is not. Clause 7.4
	 * places the loop in the implementation, and this is where the caller's
	 * copy of that loop would otherwise be. */
	if (io.e != kal_ok) return io.n ? (syscall_arg_t)io.n : -okm_errno(io.e);
	return (syscall_arg_t)io.n;
}

static syscall_arg_t do_read(int fd, void* buf, size_t len)
{
	kal_uintptr s;
	const syscall_arg_t r = stream_of(fd, &s);
	if (r) return r;
	if (len == 0) return 0;
	struct kal_stream st; st.h = s;
	const struct kal_io_result io = kal_stream_read(st, buf, len);
	if (io.e != kal_ok) return -okm_errno(io.e);
	return (syscall_arg_t)io.n;
}

/* --- opening --------------------------------------------------------------- */

static syscall_arg_t do_openat(int dirfd, const char* path, int flags, int mode)
{
	(void)mode;   /* openkal has no permission bits; see the README */
	struct okm_at at;
	syscall_arg_t r = okm_resolve(dirfd, path, &at, 0);
	if (r) return r;
	const size_t n = slen(at.rel);

	okm_lock();
	const int fd = okm_fd_alloc(0);
	if (fd < 0) { okm_unlock(); return fd; }

	if (flags & O_DIRECTORY) {
		struct kal_dir d;
		const int e = okm_fs_open_dir(at.base, at.rel, n, &d);
		if (e != kal_ok) { okm_unlock(); return -okm_errno(e); }
		struct kal_file nof = { 0 };
		okm_fd_bind(fd, OKM_DIR, 0, nof, d, flags);
		char abs[OKM_MAX_PATH];
		if (okm_absolute(dirfd, path, abs, sizeof abs) == 0) okm_dir_remember(fd, abs);
		okm_unlock();
		return fd;
	}

	kal_uintptr want = 0;
	switch (flags & O_ACCMODE) {
	case O_RDONLY: want = KAL_OPEN_READ; break;
	case O_WRONLY: want = KAL_OPEN_WRITE; break;
	default:       want = KAL_OPEN_READ | KAL_OPEN_WRITE; break;
	}
	if (flags & O_CREAT)  want |= KAL_OPEN_CREATE;
	if (flags & O_EXCL)   want |= KAL_OPEN_EXCLUSIVE;
	if (flags & O_TRUNC)  want |= KAL_OPEN_TRUNCATE;
	if (flags & O_APPEND) want |= KAL_OPEN_APPEND;

	struct kal_file f;
	const int e = okm_fs_open(at.base, at.rel, n, want, &f);
	if (e == kal_err_is_directory) {
		/* A program that opens a directory without saying so gets one, which
		 * is what a Linux program expects of O_RDONLY on a directory. */
		struct kal_dir d;
		const int e2 = okm_fs_open_dir(at.base, at.rel, n, &d);
		if (e2 != kal_ok) { okm_unlock(); return -okm_errno(e2); }
		struct kal_file nof = { 0 };
		okm_fd_bind(fd, OKM_DIR, 0, nof, d, flags);
		char abs[OKM_MAX_PATH];
		if (okm_absolute(dirfd, path, abs, sizeof abs) == 0) okm_dir_remember(fd, abs);
		okm_unlock();
		return fd;
	}
	if (e != kal_ok) { okm_unlock(); return -okm_errno(e); }
	struct kal_dir nod = { 0 };
	okm_fd_bind(fd, OKM_FILE, okm_fs_stream(f), f, nod, flags);
	okm_unlock();
	return fd;
}

/* --- enquiry --------------------------------------------------------------- */

static void fill_kstat(const struct kal_node_info* in, struct kstat* out)
{
	for (unsigned i = 0; i < sizeof *out; i++) ((char*)out)[i] = 0;
	out->st_size = (off_t)in->size;
	out->st_nlink = 1;
	out->st_blksize = OKM_PAGE;
	out->st_blocks = (blkcnt_t)((in->size + 511) / 512);
	/* openkal reports what a name refers to and whether it may be written. It
	 * does not report a permission word, an owner or a link count, because a
	 * capability-based environment has none of them to report. The mode is
	 * therefore assembled from what is known, and what is not known is left as
	 * a value a program can act upon rather than invented. */
	unsigned mode = 0;
	switch (in->kind) {
	case kal_node_file:      mode = S_IFREG; break;
	case kal_node_directory: mode = S_IFDIR; break;
	case kal_node_link:      mode = S_IFLNK; break;
	default:                 mode = S_IFCHR; break;
	}
	mode |= 0444u;
	if (in->writable) mode |= 0222u;
	if (in->kind == kal_node_directory) mode |= 0111u;
	out->st_mode = mode;
	out->st_mtime_sec  = (kal_i64)(in->modified_ns / 1000000000u);
	out->st_mtime_nsec = (kal_i64)(in->modified_ns % 1000000000u);
	out->st_atime_sec  = out->st_mtime_sec;
	out->st_atime_nsec = out->st_mtime_nsec;
	out->st_ctime_sec  = out->st_mtime_sec;
	out->st_ctime_nsec = out->st_mtime_nsec;
	out->st_uid = 1000; out->st_gid = 1000;
	out->st_ino = 0; out->st_dev = 1;
}

static syscall_arg_t do_fstat(int fd, struct kstat* st)
{
	struct okm_desc* d = okm_desc_of(fd);
	if (!d) return -EBADF;
	struct kal_node_info info;
	if (d->kind == OKM_FILE) {
		const int e = okm_fs_file_info(d->file, &info);
		if (e != kal_ok) return -okm_errno(e);
	} else if (d->kind == OKM_DIR) {
		for (unsigned i = 0; i < sizeof info; i++) ((char*)&info)[i] = 0;
		info.kind = kal_node_directory;
		info.writable = 1;
	} else {
		/* A stream that is not a file: a program asks in order to choose a
		 * buffering discipline, and what decides that is whether the stream is
		 * interactive, which openkal reports. */
		for (unsigned i = 0; i < sizeof info; i++) ((char*)&info)[i] = 0;
		info.kind = kal_node_other;
		info.writable = 1;
	}
	fill_kstat(&info, st);
	if (d->kind == OKM_STREAM) {
		struct kal_stream s; s.h = d->stream;
		st->st_mode = (kal_stream_props(s) & KAL_STREAM_PROP_INTERACTIVE)
		            ? (S_IFCHR | 0620u) : (S_IFIFO | 0600u);
	}
	return 0;
}

static syscall_arg_t do_fstatat(int dirfd, const char* path, struct kstat* st, int flag)
{
	if ((flag & AT_EMPTY_PATH) && path && !*path) return do_fstat(dirfd, st);
	struct okm_at at;
	const syscall_arg_t r = okm_resolve(dirfd, path, &at, 0);
	if (r) return r;
	struct kal_node_info info;
	const int e = okm_fs_info(at.base, at.rel, slen(at.rel), &info);
	if (e != kal_ok) return -okm_errno(e);
	if (info.kind == kal_node_absent) return -ENOENT;
	fill_kstat(&info, st);
	return 0;
}

/* --- directory enumeration -------------------------------------------------- */

struct linux_dirent64 {
	uint64_t d_ino;
	int64_t  d_off;
	uint16_t d_reclen;
	uint8_t  d_type;
	char     d_name[];
};

static syscall_arg_t do_getdents(int fd, void* buf, size_t cap)
{
	struct okm_desc* d = okm_desc_of(fd);
	if (!d) return -EBADF;
	if (d->kind != OKM_DIR) return -ENOTDIR;

	if (!d->iter_open) {
		const int e = okm_fs_list_begin(d->dir, &d->iter);
		if (e != kal_ok) return -okm_errno(e);
		d->iter_open = 1;
	}
	if (d->iter == 0) return 0;

	size_t used = 0;
	for (;;) {
		const char* name = 0; kal_uintptr len = 0; int kind = 0;
		char held[256];

		if (d->pending) {
			name = d->pending_name; kind = d->pending_kind;
			len = slen(d->pending_name);
		} else {
			const int e = okm_fs_list_next(d->dir, &d->iter, &name, &len, &kind);
			if (e != kal_ok) return used ? (syscall_arg_t)used : -okm_errno(e);
			if (!name) { d->iter = 0; break; }      /* the iterator is spent */
			if (len >= sizeof held) continue;       /* a name this layer cannot carry */
			for (kal_uintptr i = 0; i < len; i++) held[i] = name[i];
			held[len] = 0;
			name = held;
		}

		const size_t need = (sizeof(struct linux_dirent64) + len + 1 + 7) & ~(size_t)7;
		if (used + need > cap) {
			/* openkal has no operation that returns an entry to an iterator, so
			 * an entry that does not fit is held until the next call rather
			 * than dropped. A directory listing that silently lost its last
			 * entry whenever the buffer filled would be wrong in a way no
			 * caller could detect. */
			if (!d->pending) {
				for (size_t i = 0; i <= len; i++) d->pending_name[i] = name[i];
				d->pending_kind = kind;
				d->pending = 1;
			}
			if (used == 0) return -EINVAL;
			return (syscall_arg_t)used;
		}
		d->pending = 0;

		struct linux_dirent64* out = (void*)((char*)buf + used);
		out->d_ino = 1;
		out->d_off = (int64_t)(used + need);
		out->d_reclen = (uint16_t)need;
		out->d_type = kind == kal_node_directory ? DT_DIR
		            : kind == kal_node_file      ? DT_REG
		            : kind == kal_node_link      ? DT_LNK : DT_UNKNOWN;
		for (kal_uintptr i = 0; i < len; i++) out->d_name[i] = name[i];
		out->d_name[len] = 0;
		used += need;
	}
	return (syscall_arg_t)used;
}

/* --- memory ---------------------------------------------------------------- */

static syscall_arg_t do_mmap(void* addr, size_t len, int prot, int flags, int fd, off_t off)
{
	(void)prot;
	if (addr != 0 || fd >= 0 || off != 0) return -ENOSYS;
	if (!(flags & MAP_ANON) || !(flags & MAP_PRIVATE)) return -ENOSYS;
	if (len == 0) return -EINVAL;
	void* p = kal_alloc(len, OKM_PAGE);
	if (!p) return -ENOMEM;
	/* An anonymous mapping reads as zero, and a caller relies on it: musl's
	 * allocator keeps its own bookkeeping in mappings it never initialises.
	 * kal_alloc promises alignment and a size and does not promise contents ---
	 * an implementation that reuses a region cannot, without writing to memory
	 * whose contents the caller may be about to overwrite --- so the promise
	 * this layer makes is kept by this layer. */
	{
		unsigned char* q = p;
		for (size_t i = 0; i < len; i++) q[i] = 0;
	}
	return (syscall_arg_t)(uintptr_t)p;
}

/* --- time ------------------------------------------------------------------- */

static void to_timespec(kal_duration ns, struct timespec* ts)
{
	ts->tv_sec  = (time_t)(ns / 1000000000u);
	ts->tv_nsec = (long)(ns % 1000000000u);
}

/* --- processes -------------------------------------------------------------- */

#define OKM_MAX_CHILD 64
static struct { int used; int pid; struct kal_process h; } g_child[OKM_MAX_CHILD];
static int g_next_pid = 1000;

int __okm_child_record(struct kal_process h)
{
	okm_lock();
	for (int i = 0; i < OKM_MAX_CHILD; i++) {
		if (g_child[i].used) continue;
		g_child[i].used = 1;
		g_child[i].pid = ++g_next_pid;
		g_child[i].h = h;
		okm_unlock();
		return g_child[i].pid;
	}
	okm_unlock();
	return -EAGAIN;
}

static int child_index(int pid)
{
	for (int i = 0; i < OKM_MAX_CHILD; i++)
		if (g_child[i].used && g_child[i].pid == pid) return i;
	return -1;
}

static syscall_arg_t do_wait4(int pid, int* status, int options, void* rusage)
{
	(void)options; (void)rusage;
	int i = -1;
	if (pid > 0) i = child_index(pid);
	else { for (int k = 0; k < OKM_MAX_CHILD; k++) if (g_child[k].used) { i = k; break; } }
	if (i < 0) return -ECHILD;
	int st = 0, terminated = 0;
	const int e = okm_process_wait(g_child[i].h, &st, &terminated);
	if (e != kal_ok) return -okm_errno(e);
	okm_process_close(g_child[i].h);
	const int got = g_child[i].pid;
	g_child[i].used = 0;
	if (status) *status = terminated ? (st & 0x7f) : ((st & 0xff) << 8);
	return got;
}

/* --- the dispatcher --------------------------------------------------------- */

syscall_arg_t __okm_task_exit(int code);          /* okm_thread.c */
syscall_arg_t __okm_futex(const int* addr, int op, int val, const struct timespec* t);

static unsigned g_umask = 022;

syscall_arg_t __okm_syscall(syscall_arg_t n, syscall_arg_t a1, syscall_arg_t a2,
                            syscall_arg_t a3, syscall_arg_t a4, syscall_arg_t a5,
                            syscall_arg_t a6)
{
	(void)a5; (void)a6;
	switch (n) {

	/* --- transfer --------------------------------------------------------- */
	case SYS_write:  return do_write((int)a1, (const void*)a2, (size_t)a3);
	case SYS_read:   return do_read((int)a1, (void*)a2, (size_t)a3);

	case SYS_writev: {
		const struct iovec* v = (const struct iovec*)a2;
		syscall_arg_t total = 0;
		for (int i = 0; i < (int)a3; i++) {
			if (v[i].iov_len == 0) continue;
			const syscall_arg_t r = do_write((int)a1, v[i].iov_base, v[i].iov_len);
			if (r < 0) return total ? total : r;
			total += r;
			if ((size_t)r < v[i].iov_len) break;
		}
		return total;
	}
	case SYS_readv: {
		const struct iovec* v = (const struct iovec*)a2;
		syscall_arg_t total = 0;
		for (int i = 0; i < (int)a3; i++) {
			if (v[i].iov_len == 0) continue;
			const syscall_arg_t r = do_read((int)a1, v[i].iov_base, v[i].iov_len);
			if (r < 0) return total ? total : r;
			total += r;
			if ((size_t)r < v[i].iov_len) break;
		}
		return total;
	}

	/* --- naming ----------------------------------------------------------- */
	case SYS_openat: return do_openat((int)a1, (const char*)a2, (int)a3, (int)a4);
#ifdef SYS_open
	case SYS_open:   return do_openat(AT_FDCWD, (const char*)a1, (int)a2, (int)a3);
#endif
	case SYS_close: {
		okm_lock();
		struct okm_desc* d = okm_desc_of((int)a1);
		if (!d) { okm_unlock(); return -EBADF; }
		okm_fd_release((int)a1);
		okm_unlock();
		return 0;
	}
	case SYS_lseek: {
		struct okm_desc* d = okm_desc_of((int)a1);
		if (!d) return -EBADF;
		if (d->kind != OKM_FILE) return -ESPIPE;
		uint64_t at = 0;
		const int e = okm_fs_seek(d->file, (int64_t)a2, (int)a3, &at);
		if (e != kal_ok) return -okm_errno(e);
		return (syscall_arg_t)at;
	}
	/* Setting the time a file reports as its last modification.
	 *
	 * openkal states the operation on an open file, so the name is opened here
	 * and released again. It is opened for writing because openkal requires
	 * that: one of the environments beneath decides at the point of opening
	 * what may afterwards be done with a file. The consequence is a divergence
	 * from this interface's own rule --- POSIX asks for ownership and this asks
	 * for write access --- and it is recorded rather than concealed, in
	 * musl/PATCHES.md.
	 *
	 * Only the modification time is set. openkal reports one time for a file
	 * and offers to set that one; a request that leaves it alone therefore
	 * succeeds and does nothing, which is what it asked for. */
	case SYS_utimensat: {
		const struct timespec* times = (const struct timespec*)a3;
		if (times && times[1].tv_nsec == UTIME_OMIT) return 0;

		uint64_t when;
		if (!times || times[1].tv_nsec == UTIME_NOW) when = kal_time_wall();
		else when = (uint64_t)times[1].tv_sec * 1000000000u
		          + (uint64_t)times[1].tv_nsec;

		/* A null name means the descriptor itself, which is how this
		 * library expresses `futimens'. Then the file is already open and is
		 * used as it is; a file the caller opened without asking to write it
		 * is reported as such by the implementation, which is the same answer
		 * the environment would give. */
		if (!(const char*)a2) {
			struct okm_desc* d = okm_desc_of((int)a1);
			if (!d || d->kind != OKM_FILE) return -EBADF;
			const int e = okm_fs_set_modified(d->file, when);
			return e == kal_ok ? 0 : -okm_errno(e);
		}

		struct okm_at at;
		{
			const long r = okm_resolve((int)a1, (const char*)a2, &at, 0);
			if (r) return r;
		}
		struct kal_file f;
		int e = okm_fs_open(at.base, at.rel, slen(at.rel),
		                    KAL_OPEN_READ | KAL_OPEN_WRITE, &f);
		if (e != kal_ok) return -okm_errno(e);
		e = okm_fs_set_modified(f, when);
		okm_fs_close_file(f);
		return e == kal_ok ? 0 : -okm_errno(e);
	}

	case SYS_ftruncate: {
		struct okm_desc* d = okm_desc_of((int)a1);
		if (!d || d->kind != OKM_FILE) return -EBADF;
		const int e = okm_fs_truncate(d->file, (uint64_t)a2);
		return e == kal_ok ? 0 : -okm_errno(e);
	}
	case SYS_fsync:
#ifdef SYS_fdatasync
	case SYS_fdatasync:
#endif
	{
		kal_uintptr s;
		const syscall_arg_t r = stream_of((int)a1, &s);
		if (r) return r;
		struct kal_stream st; st.h = s;
		const int e = kal_stream_flush(st);
		return e == kal_ok ? 0 : -okm_errno(e);
	}

#ifdef SYS_statx
	case SYS_statx: {
		/* musl asks for this first wherever the record below carries a time
		 * narrower than the library's own, which is every target whose `long'
		 * is narrower than its pointer. Answering it is therefore not an
		 * optimisation there: the alternative is a file's time truncated to
		 * thirty-two bits with nothing reporting it. */
		struct okm_statx {
			uint32_t mask, blksize;
			uint64_t attributes;
			uint32_t nlink, uid, gid;
			uint16_t mode, pad1;
			uint64_t ino, size, blocks, attributes_mask;
			struct { int64_t sec; uint32_t nsec; int32_t pad; } atime, btime, ctime, mtime;
			uint32_t rdev_major, rdev_minor, dev_major, dev_minor;
			uint64_t spare[14];
		}* out = (void*)a5;
		struct kstat st;
		const syscall_arg_t r = ((int)a4 & AT_EMPTY_PATH) && a2 && !*(const char*)a2
			? do_fstat((int)a1, &st)
			: do_fstatat((int)a1, (const char*)a2, &st, (int)a3);
		if (r) return r;
		for (unsigned i = 0; i < sizeof *out; i++) ((char*)out)[i] = 0;
		out->mask = 0x7ff;
		out->blksize = 4096;
		out->nlink = (uint32_t)st.st_nlink;
		out->uid = (uint32_t)st.st_uid;
		out->gid = (uint32_t)st.st_gid;
		out->mode = (uint16_t)st.st_mode;
		out->ino = (uint64_t)st.st_ino;
		out->size = (uint64_t)st.st_size;
		out->blocks = (uint64_t)st.st_blocks;
		out->mtime.sec = st.st_mtime_sec; out->mtime.nsec = (uint32_t)st.st_mtime_nsec;
		out->atime = out->ctime = out->btime = out->mtime;
		return 0;
	}
#endif
	case SYS_fstat:    return do_fstat((int)a1, (struct kstat*)a2);
	case SYS_newfstatat: return do_fstatat((int)a1, (const char*)a2, (struct kstat*)a3, (int)a4);
#ifdef SYS_stat
	case SYS_stat:     return do_fstatat(AT_FDCWD, (const char*)a1, (struct kstat*)a2, 0);
#endif
#ifdef SYS_lstat
	case SYS_lstat:    return do_fstatat(AT_FDCWD, (const char*)a1, (struct kstat*)a2,
	                                     AT_SYMLINK_NOFOLLOW);
#endif
	case SYS_getdents64: return do_getdents((int)a1, (void*)a2, (size_t)a3);

	case SYS_mkdirat: {
		struct okm_at at;
		const syscall_arg_t r = okm_resolve((int)a1, (const char*)a2, &at, 0);
		if (r) return r;
		const int e = okm_fs_mkdir(at.base, at.rel, slen(at.rel));
		return e == kal_ok ? 0 : -okm_errno(e);
	}
#ifdef SYS_mkdir
	case SYS_mkdir: {
		struct okm_at at;
		const syscall_arg_t r = okm_resolve(AT_FDCWD, (const char*)a1, &at, 0);
		if (r) return r;
		const int e = okm_fs_mkdir(at.base, at.rel, slen(at.rel));
		return e == kal_ok ? 0 : -okm_errno(e);
	}
#endif
	case SYS_unlinkat: {
		struct okm_at at;
		const syscall_arg_t r = okm_resolve((int)a1, (const char*)a2, &at, 0);
		if (r) return r;
		const int e = okm_fs_remove(at.base, at.rel, slen(at.rel));
		return e == kal_ok ? 0 : -okm_errno(e);
	}
#ifdef SYS_unlink
	case SYS_unlink: {
		struct okm_at at;
		const syscall_arg_t r = okm_resolve(AT_FDCWD, (const char*)a1, &at, 0);
		if (r) return r;
		const int e = okm_fs_remove(at.base, at.rel, slen(at.rel));
		return e == kal_ok ? 0 : -okm_errno(e);
	}
#endif
#ifdef SYS_rmdir
	case SYS_rmdir: {
		struct okm_at at;
		const syscall_arg_t r = okm_resolve(AT_FDCWD, (const char*)a1, &at, 0);
		if (r) return r;
		const int e = okm_fs_remove(at.base, at.rel, slen(at.rel));
		return e == kal_ok ? 0 : -okm_errno(e);
	}
#endif
	/* One architecture states only the later of the two. The kernel's table
	 * grew, and an architecture added after the growth has no number for the
	 * operation the earlier one named --- so the pair is written as two
	 * conditionals rather than as one name and an optional second. */
#ifdef SYS_renameat
	case SYS_renameat:
#endif
#ifdef SYS_renameat2
	case SYS_renameat2:
#endif
	{
		struct okm_at from, to;
		syscall_arg_t r = okm_resolve((int)a1, (const char*)a2, &from, 0);
		if (r) return r;
		r = okm_resolve((int)a3, (const char*)a4, &to, 0);
		if (r) return r;
		const int e = okm_fs_rename(from.base, from.rel, slen(from.rel),
		                            to.base, to.rel, slen(to.rel));
		return e == kal_ok ? 0 : -okm_errno(e);
	}
#ifdef SYS_rename
	case SYS_rename: {
		struct okm_at from, to;
		syscall_arg_t r = okm_resolve(AT_FDCWD, (const char*)a1, &from, 0);
		if (r) return r;
		r = okm_resolve(AT_FDCWD, (const char*)a2, &to, 0);
		if (r) return r;
		const int e = okm_fs_rename(from.base, from.rel, slen(from.rel),
		                            to.base, to.rel, slen(to.rel));
		return e == kal_ok ? 0 : -okm_errno(e);
	}
#endif
	case SYS_faccessat:
#ifdef SYS_faccessat2
	case SYS_faccessat2:
#endif
	{
		struct okm_at at;
		const syscall_arg_t r = okm_resolve((int)a1, (const char*)a2, &at, 0);
		if (r) return r;
		struct kal_node_info info;
		const int e = okm_fs_info(at.base, at.rel, slen(at.rel), &info);
		if (e != kal_ok) return -okm_errno(e);
		if (info.kind == kal_node_absent) return -ENOENT;
		if (((int)a3 & W_OK) && !info.writable) return -EACCES;
		return 0;
	}
#ifdef SYS_access
	case SYS_access: {
		struct okm_at at;
		const syscall_arg_t r = okm_resolve(AT_FDCWD, (const char*)a1, &at, 0);
		if (r) return r;
		struct kal_node_info info;
		const int e = okm_fs_info(at.base, at.rel, slen(at.rel), &info);
		if (e != kal_ok) return -okm_errno(e);
		if (info.kind == kal_node_absent) return -ENOENT;
		if (((int)a2 & W_OK) && !info.writable) return -EACCES;
		return 0;
	}
#endif
	case SYS_getcwd: {
		const char* p = okm_cwd_path();
		const size_t n = slen(p) + 1;
		if ((size_t)a2 < n) return -ERANGE;
		char* out = (char*)a1;
		for (size_t i = 0; i < n; i++) out[i] = p[i];
		return (syscall_arg_t)n;
	}
	case SYS_chdir:  return okm_chdir(AT_FDCWD, (const char*)a1);
	case SYS_fchdir: return okm_chdir((int)a1, 0);

	case SYS_readlinkat: {
		struct okm_at at;
		const syscall_arg_t r = okm_resolve((int)a1, (const char*)a2, &at, 0);
		if (r) return r;
		struct kal_node_info info;
		const int e = okm_fs_info(at.base, at.rel, slen(at.rel), &info);
		if (e != kal_ok) return -okm_errno(e);
		if (info.kind == kal_node_absent) return -ENOENT;
		/* A name that is not a symbolic link is answered as POSIX answers it.
		 * A name that is one cannot be read: openkal reserves the operations
		 * upon links to an interface it has not defined, so the honest answer
		 * is that the operation is unavailable rather than that the link
		 * points at nothing. */
		return info.kind == kal_node_link ? -ENOSYS : -EINVAL;
	}
#ifdef SYS_readlink
	case SYS_readlink: {
		struct okm_at at;
		const syscall_arg_t r = okm_resolve(AT_FDCWD, (const char*)a1, &at, 0);
		if (r) return r;
		struct kal_node_info info;
		const int e = okm_fs_info(at.base, at.rel, slen(at.rel), &info);
		if (e != kal_ok) return -okm_errno(e);
		if (info.kind == kal_node_absent) return -ENOENT;
		return info.kind == kal_node_link ? -ENOSYS : -EINVAL;
	}
#endif

	/* --- descriptors ------------------------------------------------------ */
#ifdef SYS_dup2
	case SYS_dup2: { okm_lock(); const syscall_arg_t r = okm_fd_dup((int)a1, (int)a2, 0); okm_unlock(); return r; }
#endif
	case SYS_dup3: { okm_lock(); const syscall_arg_t r = okm_fd_dup((int)a1, (int)a2,
	                                                       ((int)a3 & O_CLOEXEC) ? 1 : 0);
	                 okm_unlock(); return r; }
	case SYS_dup:  { okm_lock(); const syscall_arg_t r = okm_fd_dup((int)a1, -1, 0); okm_unlock(); return r; }

	case SYS_fcntl: {
		struct okm_desc* d = okm_desc_of((int)a1);
		if (!d) return -EBADF;
		switch ((int)a2) {
		case F_DUPFD: {
			okm_lock();
			const int nfd = okm_fd_alloc((int)a3);
			const syscall_arg_t r = nfd < 0 ? nfd : okm_fd_dup((int)a1, nfd, 0);
			okm_unlock();
			return r;
		}
		case F_DUPFD_CLOEXEC: {
			okm_lock();
			const int nfd = okm_fd_alloc((int)a3);
			const syscall_arg_t r = nfd < 0 ? nfd : okm_fd_dup((int)a1, nfd, 1);
			okm_unlock();
			return r;
		}
		case F_GETFD: return okm_fd_get_cloexec((int)a1) ? FD_CLOEXEC : 0;
		case F_SETFD: return okm_fd_cloexec((int)a1, ((int)a3 & FD_CLOEXEC) ? 1 : 0);
		case F_GETFL: return d->flags;
		case F_SETFL: d->flags = (d->flags & ~(O_APPEND | O_NONBLOCK))
		                       | ((int)a3 & (O_APPEND | O_NONBLOCK));
		              return 0;
		case F_SETLK: case F_SETLKW: case F_GETLK: return 0;
		default: return -EINVAL;
		}
	}

	case SYS_ioctl: {
		struct okm_desc* d = okm_desc_of((int)a1);
		if (!d) return -EBADF;
		if (d->kind == OKM_STREAM || d->kind == OKM_FILE) {
			struct kal_stream s; s.h = d->stream;
			const int interactive =
				(d->kind == OKM_STREAM) &&
				(kal_stream_props(s) & KAL_STREAM_PROP_INTERACTIVE) != 0;
			/* The only question a C library asks through this call is whether
			 * the stream is a terminal, and openkal answers it. Everything a
			 * terminal can be asked to do beyond that is not an operation
			 * openkal has.
			 *
			 * ⚠️⚠️ AND THE REQUEST IT ASKS IT WITH IS NOT THE ONE THIS BRANCH
			 * FIRST RECOGNISED. `TCGETS' is the request a C library uses to
			 * READ a terminal's settings; the one it uses to ASK WHETHER
			 * something is a terminal is musl's own `isatty':
			 *
			 *     struct winsize wsz;
			 *     r = syscall(SYS_ioctl, fd, TIOCGWINSZ, &wsz);
			 *     if (r == 0) return 1;
			 *
			 * So every `isatty' over this port answered 0 --- for a real
			 * terminal as readily as for a pipe. Measured 2026-08-27 against a
			 * native control under one harness:
			 *
			 *                       pipe   pseudo-terminal
			 *     native glibc        0            1
			 *     this port           0            0
			 *
			 * ⇒ `std::print' never took its terminal path, and any program
			 * that decides on colour or on line buffering by asking decided
			 * wrongly and in silence.
			 *
			 * ⭐ THE SIZE IS REPORTED AS UNKNOWN RATHER THAN GUESSED. openkal
			 * has no operation that answers it, and `winsize' is already
			 * zeroed by the caller; a fabricated 80x24 would be this file's one
			 * forbidden shape --- reporting success having done nothing.
			 * A caller that wants the size reads zero, which is what a serial
			 * line reports too. */
			if (!interactive) return -ENOTTY;
			if ((unsigned long)a2 == TCGETS)     return 0;
			if ((unsigned long)a2 == TIOCGWINSZ) return 0;
			return -ENOTTY;
		}
		return -ENOTTY;
	}

	/* --- memory ----------------------------------------------------------- */
	case SYS_mmap:   return do_mmap((void*)a1, (size_t)a2, (int)a3, (int)a4, (int)a5, (off_t)a6);
	case SYS_munmap:
		kal_free((void*)a1, (size_t)a2, OKM_PAGE);
		return 0;
	case SYS_mprotect:
		/* openkal has no operation upon the protection of a mapping. Reporting
		 * success would be the one kind of entry this file does not contain:
		 * musl asks for a guard page below a stack, and a guard page that was
		 * claimed and not established converts a stack overflow from an
		 * immediate stop into a silent corruption of whatever lies beneath.
		 * musl already handles this answer --- it proceeds without the guard
		 * page when the call reports ENOSYS --- so the honest answer is also
		 * the one the caller is prepared for. */
		return -ENOSYS;
	case SYS_madvise: return 0;   /* advice that is not taken is still advice */
#ifdef SYS_mremap
	case SYS_mremap:  return -ENOSYS;
#endif
#ifdef SYS_brk
	case SYS_brk:     return -ENOSYS;
#endif

	/* --- time -------------------------------------------------------------- */
	case SYS_clock_gettime: {
		struct timespec* ts = (struct timespec*)a2;
		if ((int)a1 == CLOCK_REALTIME) to_timespec(kal_time_wall(), ts);
		else                            to_timespec(kal_time_monotonic(), ts);
		return 0;
	}
	case SYS_clock_getres:
		if (a2) to_timespec(kal_time_monotonic_granularity(), (struct timespec*)a2);
		return 0;
#ifdef SYS_gettimeofday
	case SYS_gettimeofday: {
		struct okm_timeval { time_t tv_sec; long tv_usec; }* tv = (void*)a1;
		if (tv) {
			const kal_duration ns = kal_time_wall();
			tv->tv_sec = (time_t)(ns / 1000000000u);
			tv->tv_usec = (long)((ns % 1000000000u) / 1000u);
		}
		return 0;
	}
#endif
#ifdef SYS_nanosleep
	case SYS_nanosleep: {
		const struct timespec* t = (const struct timespec*)a1;
		if (!t) return -EFAULT;
		kal_time_sleep((kal_duration)t->tv_sec * 1000000000u + (kal_duration)t->tv_nsec);
		if (a2) { struct timespec* rem = (struct timespec*)a2; rem->tv_sec = 0; rem->tv_nsec = 0; }
		return 0;
	}
#endif
	case SYS_clock_nanosleep: {
		const struct timespec* t = (const struct timespec*)a3;
		if (!t) return -EFAULT;
		kal_duration ns = (kal_duration)t->tv_sec * 1000000000u + (kal_duration)t->tv_nsec;
		if ((int)a2 & TIMER_ABSTIME) {
			const kal_duration now = ((int)a1 == CLOCK_REALTIME)
			                       ? kal_time_wall() : kal_time_monotonic();
			ns = ns > now ? ns - now : 0;
		}
		kal_time_sleep(ns);
		return 0;
	}

	/* --- execution contexts ------------------------------------------------ */
	case SYS_futex:
		return __okm_futex((const int*)a1, (int)a2, (int)a3, (const struct timespec*)a4);
	/* ⭐ THROUGH THE INTERFACE, NOT THROUGH THE PLATFORM.
	 *
	 * musl's own `src/linux/getrandom.c` issues SYS_getrandom directly, which
	 * is right where a Linux kernel is underneath and wrong here: this port
	 * exists so that every request reaches the environment through openkal.
	 * The call below is the whole difference.
	 *
	 * ⚠️ AND IT IS WHY `openkal.random` HAD TO EXIST. Entropy is not derivable
	 * from the other interfaces --- a clock reading is unpredictable to a
	 * reader of the source and not to an adversary, which the AT_RANDOM note
	 * in okm_start.c already says about the bytes it derives, and openkal.fs
	 * deliberately cannot open `/dev/urandom`. Neither bypassing the layer nor
	 * inventing entropy was acceptable, so the layer gained an interface.
	 *
	 * The flags argument is ignored: GRND_NONBLOCK asks for a short read and
	 * `kal_random_fill` has no partial success to report. An environment that
	 * blocks says so in `kal_random_props`.
	 *
	 * ⭐⭐ AND THE REFERENCE IS WEAK, BECAUSE THE INTERFACE IS OPTIONAL.
	 *
	 * `openkal.random` is optional, and clause 6.1 expresses an implementation
	 * that does not provide it as the absence of a link-time definition. This
	 * dispatcher is linked into every program, so a strong reference here turns
	 * that absence into a failure for programs that never ask for a random
	 * byte:
	 *
	 *     ld.lld: error: undefined symbol: kal_random_fill
	 *     >>> referenced by okm_syscall.c:409
	 *
	 * measured on a bare-metal program over openkal-opensbi, which provides
	 * eight interfaces and not this one.
	 *
	 * ⚠️ A WEAK REFERENCE IS NOT THE RUN-TIME REFUSAL CLAUSE 6.1 FORBIDS. That
	 * clause governs an IMPLEMENTATION of openkal: one shall not offer an
	 * interface whose operations report a lack of support while running. What
	 * happens below is on the other side of the layer --- this file implements
	 * Linux's system call ABI, where `ENOSYS` is that ABI's defined answer for
	 * a call the kernel does not have, and musl's own `getrandom` is written
	 * against exactly that answer.
	 *
	 * The idiom is already used in this port: see `__ehdr_start` in
	 * okm_phdr.c. */
	case SYS_getrandom: {
		if (!kal_random_fill) return -ENOSYS;
		const int rc = kal_random_fill((void*)a1, (kal_uintptr)a2);
		if (rc != kal_ok) return -okm_errno(rc);
		return (syscall_arg_t)a2;
	}
	case SYS_sched_yield: okm_task_yield(); return 0;
	case SYS_gettid:      return (syscall_arg_t)OKM_CONTEXT_ID();
	case SYS_getpid:      return 1;
	case SYS_set_tid_address: return (syscall_arg_t)OKM_CONTEXT_ID();
	case SYS_exit:        return __okm_task_exit((int)a1);
	case SYS_exit_group:  kal_exit((int)a1); return 0;

	/* --- programs ---------------------------------------------------------- */
	case SYS_wait4: return do_wait4((int)a1, (int*)a2, (int)a3, (void*)a4);

	/* Replacing the calling program with another.
	 *
	 * openkal has no such operation and the omission is deliberate: an
	 * environment that has no way to replace a running image cannot supply one,
	 * and clause 3.1 declines to simulate what cannot be supplied. What openkal
	 * has is starting a program, which every environment can do.
	 *
	 * So this is expressed as starting the program, waiting for it, and ending
	 * with the status it ended with. A caller cannot distinguish that from a
	 * replacement by anything it can observe through this library --- the same
	 * program runs, with the same arguments, on the same streams, and the same
	 * status reaches whoever waits. What differs is not observable here: there
	 * are two images where a system with the operation would have one, so the
	 * identifier the started program reports is not the caller's, and a signal
	 * sent to the caller does not reach it. This library has no signals in any
	 * case.
	 *
	 * It is the arrangement every environment without the operation uses, and
	 * two of the three beneath openkal are such environments. The divergence is
	 * recorded in musl/PATCHES.md. */
	case SYS_execve: {
		pid_t child = 0;
		const int e = __posix_spawn(&child, (const char*)a1, 0, 0,
		                            (char* const*)a2, (char* const*)a3);
		if (e) return -e;
		int st = 0;
		if (do_wait4((int)child, &st, 0, 0) < 0) kal_exit(127);
		/* The status the started program ended with, in the form this library
		 * hands to a caller of wait: the low seven bits name a signal and are
		 * zero when the program ended by returning. */
		kal_exit((st & 0x7f) ? 128 + (st & 0x7f) : ((st >> 8) & 0xff));
		return 0;
	}
#ifdef SYS_kill
	case SYS_kill: {
		const int i = child_index((int)a1);
		if (i < 0) return -ESRCH;
		const int e = okm_process_terminate(g_child[i].h);
		return e == kal_ok ? 0 : -okm_errno(e);
	}
#endif

	/* --- identity ---------------------------------------------------------- */
	/* openkal describes a boundary between a program and its environment and
	 * says nothing about who is running the program: an environment with no
	 * notion of a user has no answer to give, and one answer must be given.
	 * A value that is not the privileged one is chosen, so that a program
	 * which takes a different path when privileged takes the ordinary one. */
	case SYS_getuid: case SYS_geteuid: case SYS_getgid: case SYS_getegid:
		return 1000;
	case SYS_umask: { const syscall_arg_t old = g_umask; g_umask = (unsigned)a1 & 0777u; return old; }
	case SYS_uname: {
		struct utsname* u = (struct utsname*)a1;
		static const char* const parts[] = { "openkal", "openkal", "0.5.0", "openkal", 0 };
		char* fields[] = { u->sysname, u->nodename, u->release, u->version, u->machine };
		for (int f = 0; f < 5; f++) {
			const char* v = parts[f] ? parts[f] : "unknown";
			int i = 0; for (; v[i] && i < 64; i++) fields[f][i] = v[i];
			fields[f][i] = 0;
		}
		return 0;
	}

	/* --- signals ----------------------------------------------------------- */
	/* openkal has no asynchronous delivery. Masking a set of signals that
	 * cannot arrive is a request that is satisfied by there being nothing to
	 * mask, so it succeeds. Installing a handler is not: a handler that was
	 * accepted and can never run is exactly the silent wrongness clause 3.1
	 * names, so the request is refused and the program learns it. */
	case SYS_rt_sigprocmask: {
		sigset_t* old = (sigset_t*)a3;
		if (old) for (unsigned i = 0; i < sizeof *old; i++) ((char*)old)[i] = 0;
		return 0;
	}
	case SYS_rt_sigaction: {
		const struct { void* handler; unsigned long flags; void* restorer; }* act = (const void*)a2;
		if (a3) {
			char* old = (char*)a3;
			for (unsigned i = 0; i < sizeof *act; i++) old[i] = 0;
		}
		if (!act) return 0;
		const uintptr_t h = (uintptr_t)act->handler;
		if (h == 0 || h == 1) return 0;      /* SIG_DFL and SIG_IGN */
		return -ENOSYS;
	}
#ifdef SYS_sigaltstack
	case SYS_sigaltstack: return 0;
#endif
	case SYS_rt_sigreturn: return -ENOSYS;

	default:
		/* Everything else openkal does not have. The C library above reports
		 * the failure to the program in the way the program's author already
		 * handles, which is the outcome a missing facility should produce. */
		return -ENOSYS;
	}
}
