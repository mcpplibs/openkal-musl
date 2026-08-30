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
/* For pipe and pipe2, which are kal_process_channel. Included here rather than
 * through okm.h because this is the only source that reaches for it. */
#include <openkal/process.h>
/* ⚠️⚠️ WEAK, OR AN INTERFACE A BACKEND MAY DECLINE BECOMES ONE IT MUST PROVIDE.
 *
 * Clause 6.1 expresses an interface an implementation does not provide as the
 * absence of a definition, and a bare-metal backend provides no `openkal.process'
 * at all --- it has no second image to start. A strong reference from this port
 * would therefore make every program above it fail to link, whether or not it
 * ever asked for a pipe:
 *
 *     ld.lld: error: undefined symbol: kal_process_channel
 *     >>> referenced by okm_syscall.c:407
 *
 * ⚠️ Measured on openkal-opensbi through openkal-llvm-runtime's bare-metal row,
 * which is the row that has nothing to fall back on. The same rule is already
 * applied to `kal_random_fill' below, and it is the second time this port has
 * had to learn it. */
extern __typeof(kal_process_channel) kal_process_channel __attribute__((__weak__));
extern __typeof(kal_process_channel_close) kal_process_channel_close __attribute__((__weak__));
/* Weak, for the reason given at SYS_getrandom below: the interface is
 * optional, and an implementation that does not provide it is absent as a
 * definition rather than present and refusing. */
extern __typeof(kal_random_fill) kal_random_fill __attribute__((__weak__));
/* ⭐ WHAT `WNOHANG' IS EXPRESSED AS, AND IT WAS ALREADY IN THE SPECIFICATION.
 *
 * `waitpid' discarded its options, so a caller polling for a child that had not
 * finished BLOCKED until it did --- the one thing `WNOHANG' exists to prevent.
 * `openkal.timeout' has carried `kal_timeout_wait_process' since 0.8 and this
 * port simply never reached it.
 *
 * Weak by the same rule as everything else in that interface: a backend may
 * bound some of its resources and not others, or decline the interface. */
extern __typeof(kal_timeout_wait_process) kal_timeout_wait_process __attribute__((__weak__));

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <dirent.h>
#include <poll.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <spawn.h>
#include "kstat.h"

/* Starting a program: what SYS_execve is expressed as here. */
int __posix_spawn(pid_t* restrict, const char* restrict,
                  const posix_spawn_file_actions_t*, const posix_spawnattr_t* restrict,
                  char* const[restrict], char* const[restrict]);
/* The same, and whether the started program may outlive this one. Only
 * SYS_execve passes 1: `posix_spawn' means the opposite, because a POSIX child
 * outlives its parent. */
int __okm_spawn_common(pid_t* restrict, const char* restrict,
                       const posix_spawn_file_actions_t*, const posix_spawnattr_t* restrict,
                       char* const[restrict], char* const[restrict], int bound);

#define OKM_PAGE 4096

static size_t slen(const char* s) { size_t n = 0; while (s && s[n]) n++; return n; }

/* --- reading and writing --------------------------------------------------- */

static syscall_arg_t stream_of(int fd, kal_uintptr* out)
{
	struct okm_desc* d = okm_desc_of(fd);
	if (!d) return -EBADF;
	if (d->kind == OKM_STREAM || d->kind == OKM_CHANNEL || d->kind == OKM_FILE) {
		*out = d->stream; return 0;
	}
	/* A CONNECTION IS A STREAM AND `openkal.net' ADDS NO TRANSFER OF ITS OWN.
	 * The interface says so: `kal_stream_read' and `kal_stream_write' are the
	 * operations that move a connection's bytes. A socket that is not connected
	 * has no stream, and `d->stream' is zero there. */
	if (d->kind == OKM_SOCKET) {
		if (d->stream == 0) return -ENOTCONN;
		*out = d->stream; return 0;
	}
	return -EISDIR;
}

static syscall_arg_t do_write(int fd, const void* buf, size_t len)
{
	kal_uintptr s;
	const syscall_arg_t r = stream_of(fd, &s);
	if (r) return r;
	if (len == 0) return 0;
	struct okm_desc* d = okm_desc_of(fd);
	if (d->flags & O_NONBLOCK) return okm_timed_write(s, buf, len, OKM_NOW_NS);
	struct kal_stream st; st.h = s;
	/* ⭐ ONE SIGNED WORD, AND THIS IS THE CODE THAT ARGUED FOR IT. openkal
	 * returned a count and a condition, and every consumer of that pair --- all
	 * of them here --- collapsed it by hand and by the same rule: report what
	 * moved, or the condition when nothing did. Version 0.9 returns that. */
	const kal_intptr n = kal_stream_write(st, buf, len);
	return n < 0 ? -okm_errno((int)-n) : (syscall_arg_t)n;
}

static syscall_arg_t do_read(int fd, void* buf, size_t len)
{
	kal_uintptr s;
	const syscall_arg_t r = stream_of(fd, &s);
	if (r) return r;
	if (len == 0) return 0;

	struct okm_desc* d = okm_desc_of(fd);
	/* ⭐ WHAT A READINESS ENQUIRY TOOK IS DELIVERED HERE, and delivering it is
	 * what made that enquiry's answer true rather than momentary. okm_poll.c
	 * states why openkal leaves no other way to answer one. A short read is a
	 * result every caller of `read' already handles. */
	const long held = okm_take_ahead(d, buf, len);
	if (held == OKM_AHEAD_EOF) return 0;
	if (held) {
		/* ⚠️⚠️ AND WHATEVER ELSE IS ALREADY THERE, BECAUSE ONE BYTE ON ITS OWN
		 * TURNED EVERY POLLED READ INTO A POLLED READ OF ONE BYTE.
		 *
		 * The enquiry takes a byte to make its answer true. Returning only that
		 * byte is a legal short read --- and a caller that polls goes straight
		 * back to `poll', which takes another byte, so the whole of a stream
		 * arrives ONE BYTE PER ITERATION, for ever. Measured against the host,
		 * on `echo one; sleep 0.4; echo two':
		 *
		 *     here    "o" "n" "e" "." "t" "w" "o" "."      eight chunks
		 *     host    "one." "two."                        two
		 *
		 * ⚠️ Every byte is delivered and in order, so a caller that concatenates
		 * sees the right bytes --- which is why this survived: the defect is
		 * invisible to anyone who does not look at the BOUNDARIES. A caller that
		 * scans a chunk for a word finds none, because `two' arrives as `t' and
		 * `wo'. openkal-linux#13's first report said the program `only output
		 * one byte', and this is that.
		 *
		 * ⭐ THE REMEDY IS THE OPERATION THE ENQUIRY ITSELF IS BUILT ON. A bound
		 * of `now' asks for whatever has already arrived and does not wait, so
		 * the byte and the rest of what is there come back together. Where the
		 * environment beneath declines `openkal.timeout' there is no such
		 * operation, and the single byte is what can be honestly returned. */
		if (len > 1 && okm_can_bound()) {
			const long more = okm_timed_read(s, (char*)buf + 1, len - 1, OKM_NOW_NS);
			if (more > 0) return (syscall_arg_t)(held + more);
		}
		return (syscall_arg_t)held;
	}

	if (d->flags & O_NONBLOCK) return okm_timed_read(s, buf, len, OKM_NOW_NS);

	struct kal_stream st; st.h = s;
	const kal_intptr n = kal_stream_read(st, buf, len);
	return n < 0 ? -okm_errno((int)-n) : (syscall_arg_t)n;
}

/* --- opening --------------------------------------------------------------- */

static syscall_arg_t do_openat(int dirfd, const char* path, int flags, int mode)
{
	(void)mode;   /* openkal has no permission bits; see the README */
	struct okm_at at;
	syscall_arg_t r = okm_resolve(dirfd, path, &at, 0);
	if (r) return r;
	const size_t n = slen(at.rel);

	/* ⭐⭐ O_NOFOLLOW ON A LINK IS `ELOOP', AND ANSWERING `ENOENT' MADE A
	 * DIRECTORY UNREMOVABLE.
	 *
	 * openkal states that opening RESOLVES and offers no form that declines
	 * to --- deliberately, since a program that opens a link to read its
	 * bytes is asking what `kal_fs_link_read' answers. So this reached the
	 * link's target, and for a link whose target is absent that is `ENOENT'.
	 *
	 * ⚠️ WHICH IS A DIFFERENT ANSWER TO A DIFFERENT QUESTION. POSIX says
	 * ELOOP: `the name is a link and you said not to follow one'. ENOENT
	 * says `there is no such name', and the two are acted upon differently
	 * by exactly the caller that passes this flag.
	 *
	 * ⭐ MEASURED THROUGH THREE LAYERS. libc++'s `remove_all' descends by
	 * opening each entry O_DIRECTORY|O_NOFOLLOW: on ELOOP or ENOTDIR it
	 * unlinks the entry, on ENOENT it concludes the entry has already gone
	 * and moves on. Against this port it moved on, unlinked nothing, and
	 * then `rmdir' failed --- so `fs::remove_all' returned ENOTEMPTY and
	 * left the tree standing, while every individual operation it is built
	 * from behaved correctly. The host toolchain removed the same tree.
	 *
	 * The enquiry that answers this is the one openkal 0.9 added: ask about
	 * the name ITSELF rather than what it refers to. It is one call, on a
	 * path taken only when the caller passed the flag. */
	if (flags & O_NOFOLLOW) {
		struct kal_node_info self = { .self_size = sizeof self };
		if (okm_fs_info(at.base, at.rel, n, KAL_FS_NO_RESOLVE,
		                KAL_INFO_KIND, &self) == kal_ok
		    && self.kind == kal_node_link)
			return -ELOOP;
	}

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

	const kal_uintptr want = okm_open_flags(flags);

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
	okm_fd_bind(fd, OKM_FILE, okm_fs_stream(f).h, f, nod, flags);
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

	/* ⚠️⚠️ THE IDENTITY WAS A CONSTANT, SO EVERY FILE WAS THE SAME FILE.
	 *
	 * `st_ino' was 0 and `st_dev' was 1 for every node, and nothing reported
	 * that they were not answers. Measured through the C++ library above:
	 * `std::filesystem::equivalent("a.txt", "b.txt")' answered TRUE for two
	 * separately created files, with no error --- the one shape this port
	 * exists to refuse, arriving through a field nobody had thought of as an
	 * answer.
	 *
	 * openkal 0.9 carries it. An implementation that cannot distinguish nodes
	 * leaves the position clear, and this reports zero for both --- which is
	 * what a caller must not read as sameness, and is why `st_dev' is left at
	 * zero as well rather than at a constant that would make two unknowns
	 * compare equal. */
	if (in->present & KAL_INFO_IDENTITY) {
		out->st_dev = (dev_t)in->identity[0];
		out->st_ino = (ino_t)in->identity[1];
	} else {
		out->st_dev = 0;
		out->st_ino = 0;
	}
}

static syscall_arg_t do_fstat(int fd, struct kstat* st)
{
	struct okm_desc* d = okm_desc_of(fd);
	if (!d) return -EBADF;
	struct kal_node_info info = { .self_size = sizeof info };
	if (d->kind == OKM_FILE) {
		const int e = okm_fs_file_info(d->file, KAL_INFO_ALL, &info);
		if (e != kal_ok) return -okm_errno(e);
	} else if (d->kind == OKM_DIR) {
		for (unsigned i = 0; i < sizeof info; i++) ((char*)&info)[i] = 0;
		info.self_size = sizeof info;
		info.present   = KAL_INFO_KIND | KAL_INFO_WRITABLE;
		info.kind = kal_node_directory;
		info.writable = 1;
	} else {
		/* A stream that is not a file: a program asks in order to choose a
		 * buffering discipline, and what decides that is whether the stream is
		 * interactive, which openkal reports. */
		for (unsigned i = 0; i < sizeof info; i++) ((char*)&info)[i] = 0;
		info.self_size = sizeof info;
		info.present   = KAL_INFO_KIND | KAL_INFO_WRITABLE;
		info.kind = kal_node_other;
		info.writable = 1;
	}
	fill_kstat(&info, st);
	if (d->kind == OKM_STREAM) {
		struct kal_stream s; s.h = d->stream;
		st->st_mode = (kal_stream_props(s) & KAL_STREAM_PROP_INTERACTIVE)
		            ? (S_IFCHR | 0620u) : (S_IFIFO | 0600u);
	} else if (d->kind == OKM_CHANNEL) {
		/* A channel end is a pipe and is never a terminal, so it is reported as
		 * one without asking. Asking would be answered correctly too; not
		 * asking states that the answer is a property of the kind rather than
		 * of the resource. */
		st->st_mode = S_IFIFO | 0600u;
	}
	return 0;
}

/* Reads the content of a node that names another.
 *
 * POSIX truncates into the caller's buffer and reports what it wrote; openkal
 * reports the length the content HAS. The two are reconciled here, which is
 * where the difference belongs: a caller of `readlink' expects the first. */
static syscall_arg_t do_readlink(int dirfd, const char* path, char* out, size_t cap)
{
	struct okm_at at;
	const syscall_arg_t r = okm_resolve(dirfd, path, &at, 0);
	if (r) return r;

	/* A name that is not such a node is EINVAL, and one that is absent is
	 * ENOENT --- two answers `kal_fs_link_read' does not distinguish for us,
	 * so the enquiry is made first. It declines to resolve, because the
	 * question is about the node itself. */
	struct kal_node_info info = { .self_size = sizeof info };
	const int ie = okm_fs_info(at.base, at.rel, slen(at.rel),
	                           KAL_FS_NO_RESOLVE, KAL_INFO_KIND, &info);
	if (ie != kal_ok) return -okm_errno(ie);
	if (info.kind == kal_node_absent) return -ENOENT;
	if (info.kind != kal_node_link)   return -EINVAL;

	const kal_intptr n = okm_fs_link_read(at.base, at.rel, slen(at.rel), out, cap);
	if (n < 0) return -okm_errno((int)-n);
	return (syscall_arg_t)((size_t)n < cap ? (size_t)n : cap);
}

/* Makes a node whose content is another name.
 *
 * ⚠️ THE CONTENT IS NOT A NAME THIS INTERFACE RESOLVES. It is stored and read
 * later by whoever follows it, so it is not put through the resolution that
 * would refuse one that ascends --- and one that ascends is the ordinary case
 * for a relative target. Only the name being CREATED is resolved. */
static syscall_arg_t do_symlink(const char* target, int dirfd, const char* path)
{
	if (!target || !path) return -EFAULT;
	struct okm_at at;
	const syscall_arg_t r = okm_resolve(dirfd, path, &at, 0);
	if (r) return r;
	const int e = okm_fs_link_create(at.base, at.rel, slen(at.rel),
	                                 target, slen(target), 0);
	return e == kal_ok ? 0 : -okm_errno(e);
}

static syscall_arg_t do_fstatat(int dirfd, const char* path, struct kstat* st, int flag)
{
	if ((flag & AT_EMPTY_PATH) && path && !*path) return do_fstat(dirfd, st);
	struct okm_at at;
	const syscall_arg_t r = okm_resolve(dirfd, path, &at, 0);
	if (r) return r;
	/* ⭐⭐ `stat' AND `lstat' ARE TWO QUESTIONS AND THIS ANSWERED ONE OF THEM
	 * TWICE. The flag was ignored, so both asked about the name itself while
	 * `open' resolved --- a program was told a name referred to a link when
	 * opening it would have reached a file. Through the C++ library above:
	 * `is_regular_file' false for a name whose bytes it could read,
	 * `file_size' refused, `exists' TRUE for a name that finally referred to
	 * nothing, and one such node made a whole directory tree uncopyable.
	 *
	 * openkal 0.9 states which operations resolve, beside each of them, and
	 * `kal_fs_info' takes the choice. */
	struct kal_node_info info = { .self_size = sizeof info };
	const kal_uintptr how = (flag & AT_SYMLINK_NOFOLLOW) ? KAL_FS_NO_RESOLVE : 0;
	const int e = okm_fs_info(at.base, at.rel, slen(at.rel), how, KAL_INFO_ALL, &info);
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
			/* The name is copied into this layer's own buffer, which is what
			 * the operation now does directly --- it reports the length the
			 * name HAS, so one longer than this buffer is skipped rather than
			 * silently truncated. */
			const int e = okm_fs_list_next(d->dir, &d->iter, held, sizeof held - 1,
			                               &len, &kind);
			if (e != kal_ok) return used ? (syscall_arg_t)used : -okm_errno(e);
			if (d->iter == 0) break;                /* the iterator is spent */
			if (len >= sizeof held) continue;       /* a name this layer cannot carry */
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

/* ⚠️ THE BOUND IS PART OF THE CONTRACT, AND IT WAS NOT.
 *
 * An entry is taken when a program is started and released when it is waited
 * for, which is what a process table is; a program that starts programs and
 * never waits for them holds entries for ever, and the next start reports
 * EAGAIN. That is what POSIX says `fork' does when the table is full, so the
 * behaviour is right --- what was wrong is that the number was invisible.
 *
 * ⭐ Measured: the sixty-fifth `posix_spawn' failed with `Resource temporarily
 * unavailable' on a program that had started sixty-four and waited for none,
 * and on one that polled each with WNOHANG once and did not come back --- and a
 * caller meeting that has an error on an operation with no evident relation to
 * the ones that caused it. The README now states this bound beside the
 * descriptor and open-description bounds it already stated, and `sysconf' is
 * not the place because POSIX has no enquiry that answers it.
 *
 * The number is raised because sixty-four is small for a program that manages
 * others --- which is the kind of program that meets it --- and because each
 * entry is three words. */
#define OKM_MAX_CHILD 256
/* ⭐ AND THE UNIT THAT START FORMED, WHICH IS THE ONLY PLACE IT CAN LIVE.
 * A caller ends a group by naming a negative identifier; openkal names a unit by
 * a handle whose meaning is the implementation's. The two are related here and
 * nowhere else --- openkal deliberately offers no way to recover a unit from a
 * program, because an implementation that had to keep a table to answer that
 * would be the defect clause 7.1 describes. This library is not an
 * implementation of openkal; keeping what it was GIVEN is bookkeeping, not
 * recovery. */
static struct { int used; int pid; struct kal_process h; struct kal_job job; int has_job; } g_child[OKM_MAX_CHILD];
static int g_next_pid = 1000;

/* ⭐ WHAT THIS PROGRAM ANSWERS WHEN ASKED WHO IT IS.
 *
 * It was the constant 1 for every context, so a copy made by `fork' reported
 * the identifier of the image it was copied from --- two contexts, one answer,
 * and no way for the copy to name itself. A program that writes its identifier
 * where another will read it (a lock file, a name for a temporary, a line of a
 * log) wrote a value that named something else.
 *
 * The original keeps 1. A copy is told the identifier its parent recorded for
 * it, which is the one number both sides already agree on --- the parent's
 * `fork' returned it. */
static int g_self_pid = 1;

/* The unit this program formed, if it did. ⚠️ KEPT BECAUSE THE NUMBER A CALLER
 * WILL LATER USE IS NOT THE HANDLE: `kill(-n)' names a group by a POSIX
 * identifier, and openkal's unit is a handle whose meaning belongs to the
 * implementation. One per program, which is what one `setpgid(0, 0)' forms, so
 * this is a word and not a table --- and a copy made by `fork' carries it, which
 * is what lets a copy form the unit and its parent end it. */
static struct kal_job g_job;
static int            g_job_held;

void __okm_set_self_pid(int pid) { g_self_pid = pid; }

/* Takes an entry and settles its identifier WITHOUT a resource to put in it.
 *
 * ⚠️ THE IDENTIFIER HAS TO EXIST BEFORE THE CONTEXT DOES. `fork' copies the
 * address space at `kal_space_start', so anything the copy is to know must be
 * written before that call --- and the identifier used to be assigned after it,
 * from the handle it returned. Reserving first is what lets the copy be told.
 *
 * ⚠️ THE CALLER HOLDS THE LOCK. `__okm_child_record' takes it and this does
 * not, because `__okm_fork' is already inside it when it reserves: the copy has
 * to be taken while no other context is part-way through a change to the table.
 * A second acquisition would not nest. */
int __okm_child_reserve(int* pid_out)
{
	for (int i = 0; i < OKM_MAX_CHILD; i++) {
		if (g_child[i].used) continue;
		g_child[i].used = 1;
		g_child[i].pid = ++g_next_pid;
		g_child[i].h = (struct kal_process){ 0 };
		g_child[i].job = (struct kal_job){ 0 };
		g_child[i].has_job = 0;
		if (pid_out) *pid_out = g_child[i].pid;
		return i;
	}
	return -1;
}

void __okm_child_commit(int slot, struct kal_process h)
{
	if (slot >= 0 && slot < OKM_MAX_CHILD) g_child[slot].h = h;
}

/* The unit a start formed for this child, recorded so that a later `kill(-n)'
 * can name it. Separate from the commit above because a start forms a unit only
 * when the caller asked for one. */
void __okm_child_job(int slot, struct kal_job j)
{
	if (slot >= 0 && slot < OKM_MAX_CHILD && j.h != 0) {
		g_child[slot].job = j;
		g_child[slot].has_job = 1;
	}
}

void __okm_child_release(int slot)
{
	/* ⚠️ THE UNIT IS NOT RELEASED WITH THE PROGRAM, AND THAT IS THE POINT OF IT.
	 * A group outlives the program that formed it for exactly as long as it has
	 * members --- which is the case a caller uses a unit FOR: the shell exits
	 * immediately and the work it put in the background is what a timeout has to
	 * reach. Clearing this on the wait made `kill(-n)' answer ESRCH the moment
	 * the shell was reaped, which is a fraction of a second before every caller
	 * that wants it. Measured: the background work survived.
	 *
	 * The slot is free for a new program; the number and its unit stay until
	 * that slot is taken again. */
	if (slot >= 0 && slot < OKM_MAX_CHILD) g_child[slot].used = 0;
}

int __okm_child_record_job(struct kal_process h, struct kal_job j)
{
	okm_lock();
	int pid = 0;
	const int slot = __okm_child_reserve(&pid);
	if (slot < 0) { okm_unlock(); return -EAGAIN; }
	__okm_child_commit(slot, h);
	__okm_child_job(slot, j);
	okm_unlock();
	return pid;
}

int __okm_child_record(struct kal_process h)
{
	const struct kal_job none = { 0 };
	return __okm_child_record_job(h, none);
}

/* ⚠️ A COPY OF THE CALLING IMAGE INHERITS THIS TABLE AND MUST NOT KEEP IT.
 *
 * The entries name programs the ORIGINAL started, and POSIX is explicit that a
 * duplicate has no children. Left in place they are worse than useless: a copy
 * that called `wait' would be told about a program it did not start and cannot
 * wait for, and `system' inside a copy --- which waits for the child it just
 * started --- could be handed one of the original's instead.
 *
 * Called from okm_fork.c, in the started context, before anything reads the
 * table. The handles are not released: they belong to the original, which is
 * still holding them. */
void __okm_forget_children(void)
{
	for (int i = 0; i < OKM_MAX_CHILD; i++) g_child[i].used = 0;
}

static int child_index(int pid)
{
	for (int i = 0; i < OKM_MAX_CHILD; i++)
		if (g_child[i].used && g_child[i].pid == pid) return i;
	return -1;
}

/* The unit a program formed, which is looked up WITHOUT requiring the program to
 * still be running --- see __okm_child_release. */
static int job_index(int pid)
{
	for (int i = 0; i < OKM_MAX_CHILD; i++)
		if (g_child[i].has_job && g_child[i].pid == pid) return i;
	return -1;
}

/* --- what a signal aimed at this program means ------------------------------
 *
 * ⚠️⚠️ `abort' DID NOT END THE PROGRAM, AND WHAT ENDED IT WAS AN ILLEGAL
 * INSTRUCTION.
 *
 * musl's `raise' is one line --- `syscall(SYS_tkill, self->tid, sig)' --- and
 * this dispatcher had no case for that number, so it answered ENOSYS and the
 * program went on. `abort' is written for exactly that possibility: it raises,
 * uninstalls any handler, raises again, and then reaches the line its own
 * comment calls unreachable, `a_crash()', which on x86_64 is `hlt'. Executed
 * outside ring 0 that faults, and the fault is delivered as SIGSEGV.
 *
 * Measured 2026-08-28 on the host kernel: a program whose entire body is `hlt'
 * exits 139 with a core dumped. So every `abort' over this port --- an uncaught
 * exception, `assert', `std::terminate', `__cxa_pure_virtual',
 * `__stack_chk_fail' --- reported a segmentation fault, and a consumer reading
 * 139 was told it had a bad pointer when it had an assertion. Reported as
 * openkal-linux#13; the report reasoned from the wrong half of it, because 139
 * looked like the null jump it had also seen.
 *
 * ⭐ THE ANSWER IS NOT SYNTHESISED. openkal-linux's `kal_abort' IS
 * `tgkill(pid, tid, SIGABRT)', so routing `abort' onto it gives a real signal
 * death: a parent reads WIFSIGNALED and WTERMSIG == SIGABRT, and a core is
 * written, which is what `abort' means everywhere else. Choosing
 * `kal_exit(134)' instead would have produced a number that looks the same to a
 * shell and answers `WIFEXITED' to a program.
 *
 * ⭐⭐ AND THE TARGET IS DELIBERATELY NOT EXAMINED, WHICH IS THE OPPOSITE OF
 * WHAT IT LOOKS LIKE.
 *
 * The default action of a terminating signal ends the PROCESS and not the
 * context that was named --- that is true on Linux too --- so which context a
 * caller aimed at makes no difference to the outcome. Comparing the identifier
 * against `kal_task_current()' would have been worse than useless: a thread
 * created here records the counter in okm_thread.c rather than openkal's
 * identity, so the comparison would have failed for every context but the
 * first, and `abort' from a thread would have gone back to `hlt'.
 *
 * ⚠️⚠️ THREE NUMBERS ARE MUSL'S OWN AND MUST NOT TERMINATE ANYTHING.
 * pthread_impl.h reserves 32, 33 and 34 for the timer thread, cancellation and
 * `synccall', and each is sent with this same call. `pthread_cancel' is
 * `pthread_kill(t, SIGCANCEL)' --- so a table that made 33 a terminating signal
 * would end the program the first time anything cancelled a thread. They are
 * refused, which is what they already got and what musl already handles: this
 * port delivers no signal, so there is no handler for them to reach. */
#define OKM_SIG_MUSL_LOW  32   /* SIGTIMER   */
#define OKM_SIG_MUSL_HIGH 34   /* SIGSYNCCALL */

static syscall_arg_t signal_self(int sig)
{
	/* An enquiry rather than a request. POSIX gives it no delivery and no
	 * default action, and the answer is that this program exists. */
	if (sig == 0) return 0;
	if (sig < 0 || sig >= _NSIG) return -EINVAL;

	/* musl's own three. Not deliverable and not terminating. */
	if (sig >= OKM_SIG_MUSL_LOW && sig <= OKM_SIG_MUSL_HIGH) return -ENOSYS;

	switch (sig) {
	/* Ignored by default: complete when there is nothing to do, which is the
	 * one shape this port permits itself to report success for. */
	case SIGCHLD: case SIGURG: case SIGWINCH:
		return 0;
	/* Stopping and continuing. openkal has no operation that suspends a program
	 * and lets another resume it, and a stop that was accepted and not performed
	 * would leave a caller waiting for a state the program never entered. */
	case SIGSTOP: case SIGTSTP: case SIGTTIN: case SIGTTOU: case SIGCONT:
		return -ENOSYS;
	case SIGABRT:
		/* No message. `abort' prints nothing in any C library, and a line of
		 * this port's own on the stream a program was about to lose would be a
		 * divergence with nothing to gain: whatever had a diagnosis --- libc++abi,
		 * `assert' --- has already printed it. */
		kal_abort((const char*)0, 0);
		return 0;                      /* not reached */
	default:
		break;
	}

	/* Everything else terminates by default. The status is the form this port
	 * already hands to a caller of `wait' for a child that died on a signal, a
	 * few lines below at SYS_execve. */
	kal_exit(128 + sig);
	return 0;                          /* not reached */
}

static syscall_arg_t do_wait4(int pid, int* status, int options, void* rusage)
{
	(void)rusage;
	int i = -1;
	if (pid > 0) i = child_index(pid);
	else { for (int k = 0; k < OKM_MAX_CHILD; k++) if (g_child[k].used) { i = k; break; } }
	if (i < 0) return -ECHILD;
	int st = 0, terminated = 0;
	int e;
	if (options & WNOHANG) {
		/* ⚠️ NOT ZERO. openkal spells "no bound" as zero (timeout.h), so a
		 * caller asking not to wait must ask for the smallest bound there is
		 * and not for none. `OKM_NOW_NS' is that bound and is what this port
		 * already passes for a non-blocking read.
		 *
		 * ⚠️ An implementation rounds a bound up to its own granularity, so
		 * `WNOHANG' here waits at most one polling interval of the environment
		 * beneath rather than not at all --- a millisecond on openkal-linux,
		 * which has no bounded wait for a child and polls. Recorded in
		 * musl/PATCHES.md: it is a bound, and a bound that could be shorter than
		 * what the environment distinguishes is a promise it cannot keep. */
		if (!kal_timeout_wait_process) return -ENOSYS;

		/* ⚠️ EVERY CHILD, WHERE THE CALLER NAMED NONE. `waitpid(-1, …, WNOHANG)'
		 * asks after ANY child, and asking after the first recorded one would
		 * report "none has finished" while a later one had --- the reading a
		 * caller draining its children in a loop acts upon. The blocking form
		 * has no such choice to make: it waits, and whichever it waits for
		 * finishes eventually. */
		int found = -1;
		if (pid > 0) {
			e = kal_timeout_wait_process(g_child[i].h, OKM_NOW_NS, &st, &terminated);
			if (e != kal_err_again) found = i;
		} else {
			for (int k = 0; k < OKM_MAX_CHILD && found < 0; k++) {
				if (!g_child[k].used) continue;
				e = kal_timeout_wait_process(g_child[k].h, OKM_NOW_NS,
				                             &st, &terminated);
				if (e != kal_err_again) found = k;
			}
		}
		/* Nothing has finished. `waitpid' says so by reporting that it waited
		 * for nobody, which is a zero rather than an error, and it leaves the
		 * caller's status word alone. */
		if (found < 0) return 0;
		i = found;
	} else {
		e = okm_process_wait(g_child[i].h, &st, &terminated);
	}
	if (e != kal_ok) return -okm_errno(e);
	okm_process_close(g_child[i].h);
	const int got = g_child[i].pid;
	g_child[i].used = 0;
	if (status) *status = terminated ? (st & 0x7f) : ((st & 0xff) << 8);
	return got;
}

/* --- the report of an operation this library does not have ------------------
 *
 * ⭐ THE DEFAULT ARM ANSWERS ENOSYS IN SILENCE, AND A CONSUMER CANNOT ACT ON A
 * SILENCE.
 *
 * The answer itself is right --- POSIX has a word for a facility that is not
 * there and every caller of `open' already handles a failure. What is missing is
 * WHICH facility, and the only way to learn it has been to read this file. Two
 * rounds of openkal-linux#13 were spent on exactly that question, and reading a
 * dispatcher is not a thing a consumer of a C library should have to do.
 *
 * ⚠️ NOT ON BY DEFAULT, AND NOT A BUILD OPTION EITHER. A consumer meets this on
 * a binary it already has; rebuilding the C library to find out what the binary
 * needed is the cost this is here to remove. So it is a variable of the
 * environment, read once.
 *
 * ⚠️ AND ONLY THIS ARM. `mprotect' and `rt_sigreturn' answer ENOSYS from cases
 * of their own, and each is a decision with a reason recorded beside it rather
 * than a gap. Tracing those would report a facility as missing that this port
 * deliberately does not have, which is a different sentence.
 */
static int trace_wanted(void)
{
	/* -1 not yet asked. Asked once: a variable is looked up by walking a set. */
	static int g_want = -1;
	int want = __atomic_load_n(&g_want, __ATOMIC_ACQUIRE);
	if (want >= 0) return want;

	static const char name[]   = "OPENKAL_MUSL_TRACE";
	static const char wanted[] = "enosys";
	/* Copied into a buffer of this file's own. Nothing here allocates --- what
	 * failed may be the operation that the allocator was about to perform. */
	char v[16];
	const kal_intptr n = kal_env_var(name, sizeof name - 1, v, sizeof v);
	want = 0;
	if (n == (kal_intptr)(sizeof wanted - 1)) {
		want = 1;
		for (kal_intptr i = 0; i < n; i++)
			if (v[i] != wanted[i]) { want = 0; break; }
	}
	__atomic_store_n(&g_want, want, __ATOMIC_RELEASE);
	return want;
}

/* WHAT VERSION OF THIS LIBRARY A PROGRAM WAS BUILT AGAINST.
 *
 * build.mcpp reads it from mcpp.toml and defines it, so it is stated in one
 * place. A build tool that did not run the program, or a manifest it could not
 * read, leaves it undefined, and "unknown" is then a true statement -- unlike
 * the constant this replaces, which was "0.5.0" through every release after
 * 0.5.0 and was therefore not a missing answer but a wrong one. */
#ifndef OKM_VERSION
#define OKM_VERSION "unknown"
#endif
const char okm_version[] = OKM_VERSION;

/* THE VERSION IS ANNOUNCED WHEN THE TRACE IS ASKED FOR, NOT WHEN SOMETHING IS
 * MISSING, AND THE DIFFERENCE IS THE WHOLE POINT.
 *
 * Reported through openkal-linux#13: a consumer set this variable, saw no
 * output, and could not distinguish "the version is right and nothing is
 * absent" from "the variable did not take effect" or "this is not the binary I
 * think it is". All three printed nothing. Two rounds of that issue were spent
 * establishing which of the three had happened.
 *
 * So the line is emitted from startup, whenever the variable is set, before
 * any operation has had the chance to be missing. Nothing is emitted when it
 * is not set, which is every ordinary run. */
void okm_trace_banner(void)
{
	if (!trace_wanted()) return;

	static const char head[] = "openkal-musl ";
	char line[96];
	unsigned o = 0;
	for (unsigned i = 0; i < sizeof head - 1; i++) line[o++] = head[i];
	for (unsigned i = 0; okm_version[i] && o < sizeof line - 2; i++) line[o++] = okm_version[i];
	line[o++] = '\n';
	kal_stream_write(kal_stderr(), line, (kal_uintptr)o);
}

static void trace_absent(syscall_arg_t n)
{
	if (!trace_wanted()) return;

	/* ⚠️ EACH NUMBER ONCE, AND THE DOCUMENTATION SAYS SO. A program that retries
	 * a refused operation in a loop would otherwise bury the report in copies of
	 * itself, and a reader counting the lines would conclude it happened once. */
	if (n >= 0 && n < 8192) {
		static unsigned char seen[8192 / 8];
		const unsigned char bit = (unsigned char)(1u << (n & 7));
		if (__atomic_fetch_or(&seen[n >> 3], bit, __ATOMIC_ACQ_REL) & bit) return;
	}

	/* ⚠️ WRITTEN TO THE STREAM DIRECTLY, NOT THROUGH THIS LIBRARY'S OWN OUTPUT.
	 * What failed may be the operation that stdio was about to perform, and a
	 * report that reaches stdio from inside the failure of stdio is a report
	 * that arrives as a second failure. Nothing here allocates either. */
	static const char head[] = "openkal-musl: no operation for system call ";
	char line[80];
	unsigned o = 0;
	for (unsigned i = 0; i < sizeof head - 1; i++) line[o++] = head[i];
	unsigned long long v = (unsigned long long)(n < 0 ? -n : n);
	if (n < 0) line[o++] = '-';
	char digits[24];
	unsigned d = 0;
	do { digits[d++] = (char)('0' + (unsigned)(v % 10)); v /= 10; } while (v);
	while (d) line[o++] = digits[--d];
	line[o++] = '\n';
	kal_stream_write(kal_stderr(), line, (kal_uintptr)o);
}

/* --- the dispatcher --------------------------------------------------------- */

syscall_arg_t __okm_task_exit(int code);          /* okm_thread.c */
syscall_arg_t __okm_futex(const int* addr, int op, int val, const struct timespec* t);
syscall_arg_t __okm_fork(void);                   /* okm_fork.c   */

/* What kind of socket a descriptor is, asked through the same path a program
 * would ask through. The two message calls need it and nothing else does, so
 * the socket table is not opened up for them. */
static int sock_type_of(int fd)
{
	int t = 0;
	unsigned l = sizeof t;
	if (okm_sock_getopt(fd, SOL_SOCKET, SO_TYPE, &t, &l) < 0) return -1;
	return t;
}

/* The bound `ppoll' and `pselect' state as a timespec, in the milliseconds
 * `poll' states it in. A bound shorter than a millisecond is rounded UP to one
 * rather than down to none: rounding down would turn a wait into a poll, and
 * the interface beneath already rounds a bound up to its own granularity. */
static int ms_of_timespec(const struct timespec* ts)
{
	if (!ts) return -1;
	if (ts->tv_sec == 0 && ts->tv_nsec == 0) return 0;
	long long ms = (long long)ts->tv_sec * 1000 + ts->tv_nsec / 1000000;
	if (ms == 0) ms = 1;
	if (ms > 0x7fffffffLL) ms = 0x7fffffffLL;
	return (int)ms;
}

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
	/* copy_file_range, as a read-and-write loop.
	 *
	 * ONLY THIS ONE, AND NOT sendfile. libc++'s std::filesystem::copy_file
	 * tries copy_file_range first and falls back on a list of errors that
	 * INCLUDES ENOSYS; its fallback from sendfile accepts only EINVAL. So an
	 * implementation providing neither reaches the stream fallback through the
	 * first call, and one providing sendfile alone never gets there. Providing
	 * this one is therefore both necessary and sufficient, and providing
	 * sendfile as well would be unreachable code.
	 *
	 * A SHORT WRITE IS NOT A RESULT THIS RETURNS. openkal transfers a whole
	 * buffer or reports what prevented it, so the loop below writes everything
	 * it read before advancing, and a failure part-way is reported with the
	 * count that had already been copied --- which is what the caller needs to
	 * resume, and what the operation's own contract states.
	 *
	 * The offsets are optional and, when given, are updated rather than the
	 * descriptors' own positions. Both forms occur: std::filesystem passes
	 * offsets, and a caller copying sequentially passes none. */
	/* pipe and pipe2, upon kal_process_channel.
	 *
	 * ⭐ THIS BECAME POSSIBLE IN openkal 0.8 AND WAS NOT BEFORE. A pipe is a
	 * pair of streams of which one end is meant to cross a spawn, which is
	 * exactly what that interface provides and what openkal previously had no
	 * way to express. Until then `pipe` belonged with the facilities the port
	 * withholds; now it is supplied like any other.
	 *
	 * ⚠️ AND THE CLOSURE SAID SO BEFORE THE REASONING DID. Withholding it broke
	 * `faccessat`, which forks and reports its answer back through a pipe:
	 *
	 *     ld64.lld: error: undefined symbol: pipe2
	 *     >>> referenced by faccessat.c:45
	 *
	 * An exclusion that takes an ordinary function with it is the wrong
	 * exclusion, and that was the first evidence that this one had become so.
	 *
	 * The two ends are bound as ordinary stream descriptors, so read, write,
	 * close, dup and poll reach them through the paths they already take. */
	/* ⚠️ `SYS_pipe` DOES NOT EXIST EVERYWHERE. The architectures that gained
	 * their numbering after pipe2 have only the later call, so naming the older
	 * one unconditionally does not compile there:
	 *
	 *     error: use of undeclared identifier 'SYS_pipe'
	 *
	 * The guard is on the NUMBER being defined and not on the architecture,
	 * because what varies is the kernel's table rather than the machine. */
#ifdef SYS_pipe
	case SYS_pipe:
#endif
	case SYS_pipe2: {
		int* out = (int*)a1;
		const int flags = (n == SYS_pipe2) ? (int)a2 : 0;   /* the older call takes none */
		if (!out) return -EFAULT;
		/* O_DIRECT would ask for packet boundaries, which a stream does not
		 * have. Refusing is the honest answer; silently ignoring it would give
		 * a caller a byte stream where it asked for messages. */
		if (flags & ~(O_CLOEXEC | O_NONBLOCK)) return -EINVAL;
		/* ⚠️ AND `O_NONBLOCK' USED TO BE ACCEPTED AND CARRIED NO FURTHER. The
		 * flag was stored in the description and nothing read it, so a caller
		 * asked for a pipe that would not wait, was told it had one, and waited.
		 * That is the one shape the head of this file forbids. It is expressed
		 * now --- as the smallest bound `openkal.timeout' offers --- and refused
		 * where that interface is absent. */
		if ((flags & O_NONBLOCK) && !okm_can_bound()) return -ENOSYS;

		/* A backend that provides no `openkal.process' provides no channel,
		 * and this is where a program learns that a pipe is not available
		 * here. ENOSYS is the same answer the default branch gives for
		 * everything else openkal does not have. */
		if (!kal_process_channel) return -ENOSYS;

		struct kal_stream mine, theirs;
		const int e = kal_process_channel(&mine, &theirs);
		if (e != kal_ok) return -okm_errno(e);

		okm_lock();
		const int rfd = okm_fd_alloc(0);
		if (rfd < 0) { okm_unlock();
		               kal_process_channel_close(mine);
		               kal_process_channel_close(theirs);
		               return rfd; }
		struct kal_file nof = { 0 };
		struct kal_dir  nod = { 0 };
		okm_fd_bind(rfd, OKM_CHANNEL, mine.h, nof, nod, O_RDONLY | (flags & O_NONBLOCK));

		const int wfd = okm_fd_alloc(0);
		if (wfd < 0) { okm_unlock();
		               kal_process_channel_close(theirs);
		               return wfd; }
		okm_fd_bind(wfd, OKM_CHANNEL, theirs.h, nof, nod, O_WRONLY | (flags & O_NONBLOCK));

		if (flags & O_CLOEXEC) { okm_fd_cloexec(rfd, 1); okm_fd_cloexec(wfd, 1); }
		okm_unlock();

		out[0] = rfd;
		out[1] = wfd;
		return 0;
	}
	case SYS_copy_file_range: {
		const int fd_in = (int)a1, fd_out = (int)a3;
		int64_t* off_in  = (int64_t*)a2;
		int64_t* off_out = (int64_t*)a4;
		size_t   want    = (size_t)a5;
		const unsigned flags = (unsigned)a6;

		if (flags != 0) return -EINVAL;   /* none are defined */
		if (want == 0) return 0;

		struct okm_desc* di = okm_desc_of(fd_in);
		struct okm_desc* dobj = okm_desc_of(fd_out);
		if (!di || !dobj) return -EBADF;
		if (di->kind != OKM_FILE || dobj->kind != OKM_FILE) return -EINVAL;

		/* When an offset is given the descriptor's own position is not to
		 * move, so it is saved and put back. Seeking to learn it is the only
		 * way to ask, which is why this is done once rather than per block. */
		uint64_t keep_in = 0, keep_out = 0;
		if (off_in) {
			if (okm_fs_seek(di->file, 0, SEEK_CUR, &keep_in) != kal_ok) return -EIO;
			if (okm_fs_seek(di->file, *off_in, SEEK_SET, &(uint64_t){0}) != kal_ok) return -EIO;
		}
		if (off_out) {
			if (okm_fs_seek(dobj->file, 0, SEEK_CUR, &keep_out) != kal_ok) return -EIO;
			if (okm_fs_seek(dobj->file, *off_out, SEEK_SET, &(uint64_t){0}) != kal_ok) return -EIO;
		}

		char block[8192];
		size_t done = 0;
		syscall_arg_t failure = 0;

		while (done < want) {
			size_t chunk = want - done;
			if (chunk > sizeof block) chunk = sizeof block;

			const syscall_arg_t got = do_read(fd_in, block, chunk);
			if (got < 0) { failure = got; break; }
			if (got == 0) break;                 /* end of input */

			const syscall_arg_t put = do_write(fd_out, block, (size_t)got);
			if (put < 0) { failure = put; break; }
			done += (size_t)put;
			if (put < got) { failure = -EIO; break; }
		}

		if (off_in) {
			uint64_t now = 0;
			okm_fs_seek(di->file, 0, SEEK_CUR, &now);
			*off_in += (int64_t)done;
			okm_fs_seek(di->file, (int64_t)keep_in, SEEK_SET, &now);
		}
		if (off_out) {
			uint64_t now = 0;
			okm_fs_seek(dobj->file, 0, SEEK_CUR, &now);
			*off_out += (int64_t)done;
			okm_fs_seek(dobj->file, (int64_t)keep_out, SEEK_SET, &now);
		}

		/* A failure after some bytes moved reports the bytes, not the failure:
		 * the caller resumes from there, and reporting the error would lose
		 * what had already been written. */
		if (done > 0) return (syscall_arg_t)done;
		return failure ? failure : 0;
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
		/* ⚠️⚠️ A DIRECTORY COULD NOT HAVE ITS TIME SET, AND IT IS AN ORDINARY
		 * THING TO WANT.
		 *
		 * `KAL_OPEN_READ | KAL_OPEN_WRITE' was asked for unconditionally, and
		 * an implementation opening a directory for writing refuses --- so this
		 * answered EISDIR for every directory. Reported by a consumer whose
		 * lock is a directory it stamps; measured through the C++ library
		 * above, where BOTH overloads of `last_write_time' report under that
		 * one name, so the failure read as though READING the time had failed.
		 * Reading it was never broken.
		 *
		 * ⭐ AND THE OPERATION IS PERFORMABLE. Measured directly against
		 * openkal-linux: opening the directory with KAL_OPEN_READ succeeds and
		 * `kal_fs_set_modified' upon it succeeds and the directory's time
		 * really changes.
		 *
		 * ⚠️ WHICH IS OUTSIDE WHAT `fs.h' STATES, AND IS RECORDED RATHER THAN
		 * CONCEALED. The interface says the file "shall have been opened with
		 * KAL_OPEN_WRITE", and names `kal_fs_open_dir' --- which yields a
		 * `kal_dir' --- as the way to open a directory, while
		 * `kal_fs_set_modified' takes a `kal_file' and has no `kal_dir' form.
		 * So there is no stated route to a directory's time at all.
		 *
		 * ⭐⭐ ASKED OF THE SPECIFICATION, AND openkal 0.10 ANSWERED IT.
		 * `kal_fs_set_modified_at' takes a NAME, so a directory is now reached by
		 * a stated route rather than by opening it for reading and hoping. The
		 * older way is kept below for a backend that has not followed yet, and is
		 * tried only where the new operation is absent. */
		struct kal_node_info kind = { .self_size = sizeof kind };
		const int ke = okm_fs_info(at.base, at.rel, slen(at.rel), 0,
		                           KAL_INFO_KIND, &kind);
		/* ⚠️ AN ENQUIRY THAT CANNOT BE MADE IS NOT AN ANSWER OF `NO', so a build
		 * without `openkal.fs' asks for what the interface requires and lets the
		 * open answer, exactly as it did before this enquiry was added. */
		if (ke != kal_ok && ke != kal_err_not_supported) return -okm_errno(ke);
		if (ke == kal_ok && kind.kind == kal_node_absent) return -ENOENT;
		const kal_uintptr want = (ke == kal_ok && kind.kind == kal_node_directory)
		                       ? KAL_OPEN_READ
		                       : (KAL_OPEN_READ | KAL_OPEN_WRITE);

		{
			const int se = okm_fs_set_modified_at(at.base, at.rel,
			                                      slen(at.rel), when);
			if (se != kal_err_not_supported)
				return se == kal_ok ? 0 : -okm_errno(se);
		}

		struct kal_file f;
		int e = okm_fs_open(at.base, at.rel, slen(at.rel), want, &f);
		if (e != kal_ok) return -okm_errno(e);
		e = okm_fs_set_modified(f, when);
		okm_fs_close_file(f);
		return e == kal_ok ? 0 : -okm_errno(e);
	}

	/* SETTING THE LENGTH OF A FILE BY NAME, WHICH IS A DIFFERENT OPERATION
	 * FROM SETTING IT BY DESCRIPTOR AND WAS ABSENT.
	 *
	 * Only `ftruncate' had a case here, and `truncate' fell to the default arm.
	 * The two are not interchangeable to a caller that has a name and no open
	 * file: opening one, truncating it and releasing it is what this case does,
	 * and a caller cannot do it itself without the descriptor it has not got.
	 *
	 * The C++ library above takes the name-shaped one. libc++'s `__resize_file'
	 * is `detail::truncate(p.c_str(), size)', so `std::filesystem::resize_file'
	 * reported ENOSYS for every path. Reported in openkal-linux#13.
	 *
	 * Opened for reading AND writing rather than for writing alone. openkal
	 * decides at the point of opening what may afterwards be done with a file,
	 * and `kal_fs_truncate' is a change --- which is the same reason
	 * `SYS_utimensat' above opens the way it does. The divergence this produces
	 * from POSIX, which asks for write permission and not for read, is the one
	 * already recorded there. */
	case SYS_truncate: {
		struct okm_at at;
		{
			const long r = okm_resolve(AT_FDCWD, (const char*)a1, &at, 0);
			if (r) return r;
		}
		struct kal_file f;
		int e = okm_fs_open(at.base, at.rel, slen(at.rel),
		                    KAL_OPEN_READ | KAL_OPEN_WRITE, &f);
		if (e != kal_ok) return -okm_errno(e);
		e = okm_fs_truncate(f, (uint64_t)a2);
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
		struct kal_node_info info = { .self_size = sizeof info };
		const int e = okm_fs_info(at.base, at.rel, slen(at.rel), 0,
		                          KAL_INFO_KIND | KAL_INFO_WRITABLE, &info);
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
		struct kal_node_info info = { .self_size = sizeof info };
		const int e = okm_fs_info(at.base, at.rel, slen(at.rel), 0,
		                          KAL_INFO_KIND | KAL_INFO_WRITABLE, &info);
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

	/* ⭐ NODES WHOSE CONTENT IS ANOTHER NAME. openkal 0.9 carries the two
	 * operations, so these answer rather than refusing.
	 *
	 * ⚠️ THE ENQUIRY IS ASKED FIRST AND IT TAKES THE DIRECTORY, which is the
	 * whole reason these can be operations of `openkal.fs' at all: the same
	 * implementation succeeds on one volume and fails on another, so a caller
	 * that could not ask would be left to discover it by the attempt. */
	case SYS_readlinkat: return do_readlink((int)a1, (const char*)a2,
	                                        (char*)a3, (size_t)a4);
#ifdef SYS_readlink
	case SYS_readlink: return do_readlink(AT_FDCWD, (const char*)a1,
	                                      (char*)a2, (size_t)a3);
#endif
	case SYS_symlinkat: return do_symlink((const char*)a1, (int)a2, (const char*)a3);
#ifdef SYS_symlink
	case SYS_symlink:   return do_symlink((const char*)a1, AT_FDCWD, (const char*)a2);
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
		case F_SETFL: {
			const int want = (int)a3;
			/* ⚠️ REFUSED WHERE IT CANNOT BE HONOURED, AND ONLY WHEN IT IS BEING
			 * ASKED FOR. A descriptor that was asked to be non-blocking and is
			 * not would make every subsequent transfer wait where the caller
			 * arranged not to. `O_NONBLOCK' is expressed here as the smallest
			 * bound `openkal.timeout' offers, so a backend that declines that
			 * interface cannot express it at all --- and a caller CLEARING the
			 * flag is asking for what such a backend always gives. */
			if ((want & O_NONBLOCK) && !(d->flags & O_NONBLOCK) && !okm_can_bound())
				return -ENOSYS;
			d->flags = (d->flags & ~(O_APPEND | O_NONBLOCK))
			         | (want & (O_APPEND | O_NONBLOCK));
			return 0;
		}
		/* ⚠️⚠️ THESE THREE ANSWERED `0' AND DID NOTHING, SO EVERY LOCK WAS
		 * GRANTED AND NO LOCK EXISTED. Measured with the host as control: two
		 * programs took one exclusive lock and BOTH were told they had it.
		 *
		 * 0.10.0 refused them, and said the refusal was TEMPORARY in a way the
		 * permission one is not --- every environment beneath openkal can lock a
		 * byte range, and what was missing was a word in the specification.
		 *
		 * ⭐ openkal 0.10 IS THAT WORD. `kal_fs_lock' states the holder as the
		 * open FILE and requires release when the program ends however it ends,
		 * which is the half a caller could never have built for itself. */
		case F_SETLK: case F_SETLKW: {
			if (d->kind != OKM_FILE) return -EBADF;
			const struct flock* fl = (const struct flock*)a3;
			if (!fl) return -EFAULT;
			/* openkal takes a position and a length; POSIX takes a position, a
			 * length and where the position is measured from. Only the third
			 * needs translating, and only two of its three forms can be. */
			kal_u64 start;
			if (fl->l_whence == SEEK_SET) start = (kal_u64)fl->l_start;
			else if (fl->l_whence == SEEK_CUR) {
				kal_u64 here = 0;
				if (okm_fs_seek(d->file, 0, KAL_SEEK_CURRENT, &here) != kal_ok)
					return -EINVAL;
				start = here + (kal_u64)fl->l_start;
			} else {
				/* SEEK_END, which openkal has no form of: the length a range is
				 * measured back from is not a thing this interface reports at the
				 * moment the lock is taken. Refused rather than computed from a
				 * size that may already have changed. */
				return -EINVAL;
			}
			/* ⭐ ZERO MEANS `TO THE END, HOWEVER FAR THAT COMES TO BE' IN BOTH,
			 * so it is passed rather than translated. */
			const kal_u64 len = (kal_u64)fl->l_len;

			if (fl->l_type == F_UNLCK) {
				const int e = okm_fs_unlock(d->file, start, len);
				return e == kal_ok ? 0 : -okm_errno(e);
			}
			kal_uintptr mode = (fl->l_type == F_RDLCK) ? KAL_LOCK_SHARED
			                                           : KAL_LOCK_EXCLUSIVE;
			if ((int)a2 == F_SETLKW) mode |= KAL_LOCK_WAIT;
			const int e = okm_fs_lock(d->file, start, len, mode);
			return e == kal_ok ? 0 : -okm_errno(e);
		}
		/* ⚠️ AND THE ENQUIRY IS STILL REFUSED, WHICH IS NOT AN OVERSIGHT.
		 *
		 * `F_GETLK' asks whether a lock WOULD block without taking one, and
		 * openkal has no operation that answers a question without performing
		 * it --- the same absence clause 6.3 records for readiness. Taking the
		 * lock and releasing it again would answer, and would also take a lock
		 * the caller did not ask for, hand a spurious `no' to a caller that
		 * already holds one, and be stale the moment it returned.
		 *
		 * ⭐ A refusal is what a caller can act upon; `F_SETLK' answers the
		 * question `F_GETLK' is usually asked in order to answer. */
		case F_GETLK: return -ENOSYS;
		default: return -EINVAL;
		}
	}

	case SYS_ioctl: {
		struct okm_desc* d = okm_desc_of((int)a1);
		if (!d) return -EBADF;
		if (d->kind == OKM_STREAM || d->kind == OKM_CHANNEL || d->kind == OKM_FILE) {
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
	case SYS_getpid:      return (syscall_arg_t)g_self_pid;

	/* ⚠️⚠️ THE SAME DEFECT `getpgrp' HAD, IN THE SAME FAMILY, MISSED ONCE.
	 *
	 * musl's `getppid' is `return __syscall(SYS_getppid);' WITHOUT
	 * `__syscall_ret', deliberately, because POSIX says the call cannot fail.
	 * There was no case for the number, so the default arm answered -ENOSYS and
	 * a program that asked was told its parent was -38: not -1, no errno, and
	 * nothing to check. Measured. The note at `getpgid' below records the same
	 * shape being fixed for `getpgrp' --- and this one was three lines away and
	 * was not looked for. The criterion added with this change sweeps the whole
	 * family rather than this member of it.
	 *
	 * ⭐ ZERO RATHER THAN ONE. openkal names nothing that started this program,
	 * so there is no identifier to give. Zero is what the first process of a
	 * system answers on the environment this library's callers come from, and
	 * it means what is true here: there is no parent to name. One would be a
	 * worse answer than a wrong number, because `getppid() == 1' is read by
	 * daemonising code as "my parent has died and I have been adopted", which
	 * would send a program down a path nothing here asked for. */
	case SYS_getppid:     return 0;
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
		/* ⭐ BOUND, WHICH IS THE THING `execve' MEANS. The started program
		 * stands in for this one, so it does not outlive it --- and until
		 * openkal 0.10 there was no way to say so, which is why a `kill' aimed
		 * at this image reached the copy that waits and left the program
		 * running to completion, unsupervised. */
		const int e = __okm_spawn_common(&child, (const char*)a1, 0, 0,
		                                 (char* const*)a2, (char* const*)a3, 1);
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
		const int pid = (int)a1, sig = (int)a2;
		/* ⭐⭐ A NEGATIVE IDENTIFIER NAMES A UNIT, AND THIS IS THE OTHER HALF OF
		 * `setpgid(0, 0)' ABOVE.
		 *
		 * `kill(-n)' is how a caller ends a group, and the identifier it uses is
		 * the one `setpgid' gave it --- either this program's, or a child's whose
		 * copy formed the unit. Both reach the unit this library holds, because
		 * a program forms at most one and a copy carries its parent's.
		 *
		 * ⚠️ WITHOUT THIS THE FORMING WOULD BE INVISIBLE. That is the shape of
		 * the two defects before it: a call that succeeds and changes nothing
		 * observable is worse than one that refuses, because the caller proceeds.
		 * `kill(-n)' answered ESRCH here while the unit existed. */
		if (pid < 0 && pid != -1) {
			const int gi = job_index(-pid);
			if (gi >= 0 && g_child[gi].has_job) {
				if (sig == 0) return 0;
				const int e = okm_process_job_terminate(g_child[gi].job);
				return e == kal_ok ? 0 : -okm_errno(e);
			}
			if (g_job_held) {
				if (sig == 0) return 0;
				const int e = okm_process_job_terminate(g_job);
				return e == kal_ok ? 0 : -okm_errno(e);
			}
		}
		const int i = child_index(pid);
		if (i >= 0) {
			/* ⚠️ SIGNAL ZERO IS AN ENQUIRY AND USED TO TERMINATE THE CHILD.
			 * Every value reached `kal_process_terminate', so the one form of
			 * `kill' whose whole purpose is to change nothing --- the test that
			 * a program is still there --- killed it. */
			if (sig == 0) return 0;
			const int e = okm_process_terminate(g_child[i].h);
			return e == kal_ok ? 0 : -okm_errno(e);
		}
		/* This program itself. `getpid' answers 1 here and child identifiers
		 * begin at 1001, so `kill(getpid(), …)' reached neither branch and
		 * reported ESRCH --- including for SIGABRT. Zero and minus one name
		 * groups that contain this program, and it is the only member this
		 * library can reach. */
		/* ⚠️ AND IT IS `g_self_pid' RATHER THAN THE CONSTANT. A copy made by
		 * `fork' answers the identifier its parent recorded, so comparing
		 * against 1 would make `raise' and `abort' report ESRCH in every copy. */
		if (pid == g_self_pid || pid == 0 || pid == -1) return signal_self(sig);
		return -ESRCH;
	}
#endif

	/* Aimed at one execution context rather than at the program. musl uses both
	 * spellings: `raise' and `abort' issue the first, `pthread_kill' the second
	 * by way of the first. signal_self records why the identifier is not
	 * examined and which three numbers must never terminate anything. */
#ifdef SYS_tkill
	case SYS_tkill:  return signal_self((int)a2);
#endif
#ifdef SYS_tgkill
	case SYS_tgkill: return signal_self((int)a3);
#endif

	/* --- duplicating the calling image -------------------------------------- */
	/* ⭐ `fork' IS COMPOSED ABOVE `openkal.space' AND THE SPECIFICATION SAYS SO.
	 * okm_fork.c carries the composition and the header it quotes. What reaches
	 * here is musl's `_Fork', which issues this call with a termination signal
	 * and no stack; every other shape asks for a context that SHARES the
	 * caller's address space, which is `openkal.task' and reaches this library
	 * through `__clone' rather than through this seam. */
	/* ⚠️ TWO NUMBERS AND NOT ONE, AND THE SECOND IS THE ONE THAT MATTERED.
	 * musl's `_Fork' issues `SYS_fork' where the architecture has it and
	 * `SYS_clone' where it does not, so an implementation of the second alone
	 * is reached on aarch64 and riscv64 and never on x86_64. Measured: the
	 * probe reported `the calling image is duplicated (errno=38)' on the one
	 * architecture that has both. */
#ifdef SYS_fork
	case SYS_fork: return __okm_fork();
#endif
	case SYS_clone: {
		const unsigned long flags = (unsigned long)a1;
		if ((flags & ~0xffUL) != 0 || (void*)a2 != 0) return -ENOSYS;
		return __okm_fork();
	}

	/* --- the network -------------------------------------------------------- */
	/* Every one of these is okm_net.c's, which holds the whole of the deferral
	 * that BSD's `socket' and openkal's `kal_net_connect' differ by. */
	case SYS_socket:  return okm_sock_open((int)a1, (int)a2, (int)a3);
	case SYS_bind:    return okm_sock_bind((int)a1, (const void*)a2, (unsigned)a3);
	case SYS_listen:  return okm_sock_listen((int)a1, (int)a2);
	case SYS_accept:  return okm_sock_accept((int)a1, (void*)a2, (unsigned*)a3, 0);
	case SYS_accept4: return okm_sock_accept((int)a1, (void*)a2, (unsigned*)a3, (int)a4);
	case SYS_connect: return okm_sock_connect((int)a1, (const void*)a2, (unsigned)a3);
	case SYS_getsockname: return okm_sock_name((int)a1, (void*)a2, (unsigned*)a3, 0);
	case SYS_getpeername: return okm_sock_name((int)a1, (void*)a2, (unsigned*)a3, 1);
	case SYS_shutdown:    return okm_sock_shutdown((int)a1, (int)a2);
	case SYS_setsockopt:
		return okm_sock_setopt((int)a1, (int)a2, (int)a3, (const void*)a4, (unsigned)a5);
	case SYS_getsockopt:
		return okm_sock_getopt((int)a1, (int)a2, (int)a3, (void*)a4, (unsigned*)a5);
	case SYS_sendto:
		return okm_sock_send((int)a1, (const void*)a2, (size_t)a3, (int)a4,
		                     (const void*)a5, (unsigned)a6);
	case SYS_recvfrom:
		return okm_sock_recv((int)a1, (void*)a2, (size_t)a3, (int)a4,
		                     (void*)a5, (unsigned*)a6);

	/* The two calls that carry a vector of buffers and a place for ancillary
	 * data. openkal has no ancillary data --- there is no operation that passes
	 * a handle along a connection --- so a request carrying any is refused
	 * rather than performed without it. */
	case SYS_sendmsg: {
		const struct msghdr* m = (const struct msghdr*)a2;
		if (!m) return -EFAULT;
		if (m->msg_controllen != 0) return -ENOSYS;
		const int t = sock_type_of((int)a1);
		if (t < 0) return -ENOTSOCK;

		if (t == SOCK_DGRAM) {
			/* A MESSAGE IS SENT WHOLE OR NOT AT ALL, which openkal.datagram
			 * states. Gathering several buffers into one message would need a
			 * buffer this port has no place for, so a vector with more than one
			 * occupied entry is refused instead of being sent as several
			 * messages --- which is a different thing from what the caller
			 * asked for and would look like success. */
			int used = 0, idx = 0;
			for (int i = 0; i < (int)m->msg_iovlen; i++)
				if (m->msg_iov[i].iov_len) { used++; idx = i; }
			if (used > 1) return -ENOSYS;
			return okm_sock_send((int)a1,
			                     used ? m->msg_iov[idx].iov_base : 0,
			                     used ? m->msg_iov[idx].iov_len : 0,
			                     (int)a3, m->msg_name, m->msg_namelen);
		}

		syscall_arg_t total = 0;
		for (int i = 0; i < (int)m->msg_iovlen; i++) {
			if (m->msg_iov[i].iov_len == 0) continue;
			const syscall_arg_t r = okm_sock_send((int)a1, m->msg_iov[i].iov_base,
			                                      m->msg_iov[i].iov_len, (int)a3, 0, 0);
			if (r < 0) return total ? total : r;
			total += r;
			if ((size_t)r < m->msg_iov[i].iov_len) break;
		}
		return total;
	}
	case SYS_recvmsg: {
		struct msghdr* m = (struct msghdr*)a2;
		if (!m) return -EFAULT;
		if (m->msg_controllen != 0) return -ENOSYS;
		m->msg_flags = 0;
		const int t = sock_type_of((int)a1);
		if (t < 0) return -ENOTSOCK;

		if (t == SOCK_DGRAM) {
			int used = 0, idx = 0;
			for (int i = 0; i < (int)m->msg_iovlen; i++)
				if (m->msg_iov[i].iov_len) { used++; idx = i; }
			if (used > 1) return -ENOSYS;
			return okm_sock_recv((int)a1,
			                     used ? m->msg_iov[idx].iov_base : 0,
			                     used ? m->msg_iov[idx].iov_len : 0,
			                     (int)a3, m->msg_name, &m->msg_namelen);
		}

		if (m->msg_name) m->msg_namelen = 0;
		syscall_arg_t total = 0;
		for (int i = 0; i < (int)m->msg_iovlen; i++) {
			if (m->msg_iov[i].iov_len == 0) continue;
			const syscall_arg_t r = okm_sock_recv((int)a1, m->msg_iov[i].iov_base,
			                                      m->msg_iov[i].iov_len, (int)a3, 0, 0);
			if (r < 0) return total ? total : r;
			total += r;
			if ((size_t)r < m->msg_iov[i].iov_len) break;
		}
		return total;
	}

	/* --- readiness ---------------------------------------------------------- */
	/* okm_poll.c holds the whole of what openkal permits here, and the reason
	 * `epoll' is still withheld: a readiness SET is a facility of one kernel,
	 * and asking each descriptor in turn is what an interface without one
	 * offers. */
#ifdef SYS_poll
	case SYS_poll: return okm_poll((void*)a1, (unsigned long)a2, (int)a3);
#endif
	case SYS_ppoll:
		return okm_poll((void*)a1, (unsigned long)a2,
		                ms_of_timespec((const struct timespec*)a3));

	/* `select', which musl expresses as this call and this port expresses as
	 * `poll'. The two describe the same question in two shapes and openkal
	 * answers one of them. */
#ifdef SYS_select
	case SYS_select:
#endif
	case SYS_pselect6: {
		const int nfds = (int)a1;
		fd_set* rd = (fd_set*)a2;
		fd_set* wr = (fd_set*)a3;
		fd_set* ex = (fd_set*)a4;
		if (nfds < 0 || nfds > FD_SETSIZE) return -EINVAL;

		/* ⚠️ A BOUND ON THE SET, STATED RATHER THAN SILENT. Each descriptor in
		 * the set costs one bounded operation per round, and the set has to be
		 * held somewhere while that happens. A larger one is refused; it is not
		 * truncated, because a `select' that watched some of what it was given
		 * would report the others as never ready. */
		enum { OKM_SELECT_MAX = 128 };
		struct pollfd p[OKM_SELECT_MAX];
		int watched = 0;
		for (int fd = 0; fd < nfds; fd++) {
			short ev = 0;
			if (rd && FD_ISSET(fd, rd)) ev |= POLLIN;
			if (wr && FD_ISSET(fd, wr)) ev |= POLLOUT;
			/* An exceptional condition is out-of-band data, which openkal does
			 * not have. A descriptor named only there can never be reported
			 * ready, and is therefore not watched. */
			if (!ev) continue;
			if (watched == OKM_SELECT_MAX) return -EINVAL;
			p[watched].fd      = fd;
			p[watched].events  = ev;
			p[watched].revents = 0;
			watched++;
		}

		/* ⚠️ THE TWO CALLS STATE THE BOUND IN DIFFERENT STRUCTURES, and which
		 * one was written is decided by the number rather than by the machine:
		 * `select' passes a `timeval' and `pselect6' a `timespec'. Reading one
		 * as the other would misread the fractional field by a factor of a
		 * thousand, in silence. */
		int ms;
#ifdef SYS_select
		if (n == SYS_select) {
			const struct timeval* tv = (const struct timeval*)a5;
			struct timespec conv;
			if (!tv) ms = -1;
			else {
				conv.tv_sec  = tv->tv_sec;
				conv.tv_nsec = (long)tv->tv_usec * 1000;
				ms = ms_of_timespec(&conv);
			}
		} else
#endif
		ms = ms_of_timespec((const struct timespec*)a5);

		const long r = okm_poll(p, (unsigned long)watched, ms);
		if (r < 0) return r;

		if (rd) FD_ZERO(rd);
		if (wr) FD_ZERO(wr);
		if (ex) FD_ZERO(ex);
		long count = 0;
		for (int i = 0; i < watched; i++) {
			if (rd && (p[i].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)))
				{ FD_SET(p[i].fd, rd); count++; }
			if (wr && (p[i].revents & (POLLOUT | POLLERR | POLLNVAL)))
				{ FD_SET(p[i].fd, wr); count++; }
		}
		return count;
	}

	/* --- identity ---------------------------------------------------------- */
	/* openkal describes a boundary between a program and its environment and
	 * says nothing about who is running the program: an environment with no
	 * notion of a user has no answer to give, and one answer must be given.
	 * A value that is not the privileged one is chosen, so that a program
	 * which takes a different path when privileged takes the ordinary one. */
	case SYS_getuid: case SYS_geteuid: case SYS_getgid: case SYS_getegid:
		return 1000;

	/* ⚠️ `getpgrp' HANDED A NEGATED ERROR TO ITS CALLER AS A PROCESS GROUP.
	 *
	 * There was no case for this number, so the default arm answered -ENOSYS
	 * --- and musl's `getpgrp' is `return __syscall(SYS_getpgid, 0);' WITHOUT
	 * `__syscall_ret', deliberately, because POSIX says the call cannot fail.
	 * So a program that asked was told its group was -38: not -1, no errno,
	 * and nothing to check. Measured.
	 *
	 * openkal has no process groups and no sessions --- `kill' resolves
	 * children only --- so the honest answer for the calling program is the
	 * identity `getpid' already reports, which is the whole truth here: there
	 * is one program and it is in its own group. A group that is not this
	 * program's is a group this environment has no way to name, and is
	 * refused.
	 *
	 * ⚠️⚠️ THIS PARAGRAPH USED TO END "`setpgid' AND `setsid' REMAIN REFUSED:
	 * MAKING A GROUP IS NOT THE SAME AS BEING IN ONE", AND THAT ANSWERED A
	 * QUESTION NEITHER OF THEM ASKS.
	 *
	 * `setpgid(0, 0)' does not ask for a group to be made. It asks for the
	 * calling program to be in a group of its own --- which, by the three
	 * answers immediately below, IS ALREADY TRUE HERE. Refusing it reported
	 * that an effect was unavailable while the effect held. ⇒ It succeeds, and
	 * that is not reporting an effect that does not exist: it is reporting one
	 * that does.
	 *
	 * `setsid()' has a failure POSIX writes down: EPERM when the caller is
	 * already a process group leader. `getpgid(0) == getpid()' is that
	 * assertion, so EPERM is the true answer rather than a polite one.
	 *
	 * ⭐ AND THE DIFFERENCE IS NOT COSMETIC. The `fork'-then-`setsid' dance
	 * exists BECAUSE of EPERM, so every daemonising library handles it; not one
	 * handles ENOSYS. A written-down failure is one a caller can act on. */
	case SYS_getpgid:
		return (a1 == 0 || a1 == g_self_pid) ? g_self_pid : -ESRCH;
#ifdef SYS_getpgrp
	case SYS_getpgrp: return (syscall_arg_t)g_self_pid;
#endif
	case SYS_getsid:
		return (a1 == 0 || a1 == g_self_pid) ? g_self_pid : -ESRCH;

	/* ⭐⭐ A REAL GROUP SINCE 0.12, AND IT IS THE CALL A SHELL RUNNER MAKES.
	 *
	 * `setpgid(0, 0)' asks that THIS program lead a unit of its own, which
	 * openkal 0.11 spells `kal_process_job_enter'. It used to answer 0 and form
	 * nothing --- true in a world with no groups, since a program is then
	 * trivially alone --- and the caller's next act, `kill(-pid)', found nothing
	 * to kill.
	 *
	 * ⚠️ THE UNIT IS KEPT, BECAUSE THE NUMBER THE CALLER WILL USE IS NOT ENOUGH.
	 * A caller ends a group by naming a negative identifier, and openkal's unit
	 * is a handle whose meaning is the implementation's --- a process group's
	 * identifier on one system, a job object on another. `g_job' is where this
	 * library remembers which unit `setpgid' formed, so that `kill(-n)' below can
	 * name it. One unit per program, which is what one `setpgid(0, 0)' forms.
	 *
	 * Naming another program's group is still refused: openkal cannot put a
	 * program into a unit it does not lead, and EPERM says the operation is here
	 * while the group is not. */
	case SYS_setpgid:
		if (a1 != 0 && a1 != (syscall_arg_t)g_self_pid) return -EPERM;
		if (a2 == 0 || a2 == (syscall_arg_t)g_self_pid) {
			struct kal_job unit = { 0 };
			const int e = okm_process_job_enter(&unit);
			if (e == kal_ok) { g_job = unit; g_job_held = 1; return 0; }
			/* An environment with no units leaves a program trivially alone in
			 * one, which is what this used to assert unconditionally. */
			if (e == kal_err_not_supported) return 0;
			return -okm_errno(e);
		}
		return -EPERM;
	case SYS_setsid:
		return -EPERM;   /* already a process group leader; see above */

	/* ⭐ THE BOUND IS THIS LIBRARY'S OWN AND IT WAS REFUSING TO STATE IT.
	 *
	 * musl answers `sysconf(_SC_OPEN_MAX)' from this call, so with no case here
	 * the answer was ZERO --- and a program sizing a set of descriptors, or
	 * closing every descriptor above a point, acts on that number. The number
	 * is not unknown: it is OKM_MAX_FD, which README.md states beside the other
	 * bounds this port fixes.
	 *
	 * Only the one resource is answered. The others are not refused out of
	 * caution but out of not knowing: openkal reports no address-space or
	 * processor limits, and inventing one would be the shape this port exists
	 * to avoid. Setting any limit is refused for the same reason.
	 *
	 * musl reaches this through `prlimit64' first and falls back to
	 * `getrlimit' only on ENOSYS, so answering this one answers both. */
	/* ⭐⭐ IT ANSWERED 1, SILENTLY, AND A POOL OF WORKERS WAS SIZED AGAINST IT.
	 *
	 * musl's `sysconf(_SC_NPROCESSORS_ONLN)' reaches this, and with no case it
	 * fell back to 1 --- so `std::thread::hardware_concurrency()' answered 1
	 * with no error and a program that sizes itself got one worker and no way
	 * to know. Measured: 1 here against 32 on the same machine's own C library.
	 * openkal 0.10 adds the enquiry, and it reports the set THIS context may
	 * run on rather than the set the machine has.
	 *
	 * ⚠️ ZERO IS `CANNOT SAY' AND IS NOT ONE, so it is reported as a refusal
	 * rather than as a bitmap of one processor: musl would read the latter as a
	 * fact and this port would be inventing it. */
#ifdef SYS_sched_getaffinity
	case SYS_sched_getaffinity: {
		const size_t cap = (size_t)a2;
		unsigned char* out = (unsigned char*)a3;
		if (!out || cap < sizeof(unsigned long)) return -EINVAL;
		if (!okm_task_parallelism) return -ENOSYS;
		const kal_uintptr n = okm_task_parallelism();
		if (n == 0) return -ENOSYS;

		for (size_t i = 0; i < cap; i++) out[i] = 0;
		/* The identity of the processors is not reported by openkal --- only how
		 * many --- so the lowest `n' positions are set. musl counts the bits and
		 * asks nothing else of them. */
		size_t bits = (size_t)n;
		if (bits > cap * 8) bits = cap * 8;
		for (size_t i = 0; i < bits; i++) out[i / 8] |= (unsigned char)(1u << (i % 8));
		const size_t words = (bits + 8 * sizeof(unsigned long) - 1)
		                   / (8 * sizeof(unsigned long));
		return (syscall_arg_t)(words * sizeof(unsigned long));
	}
#endif

	/* How much the volume holds, which `std::filesystem::space' reaches. */
#if defined(SYS_statfs) || defined(SYS_fstatfs)
	case
#  ifdef SYS_statfs
	     SYS_statfs
#  else
	     SYS_fstatfs
#  endif
	: {
		struct okm_statfs {
			unsigned long type, bsize;
			uint64_t blocks, bfree, bavail, files, ffree;
			struct { int v[2]; } fsid;
			unsigned long namelen, frsize, flags, spare[4];
		}* out = (void*)a2;
		if (!out) return -EFAULT;
		struct okm_at at;
		const syscall_arg_t r = okm_resolve(AT_FDCWD, (const char*)a1, &at, 0);
		if (r) return r;
		struct kal_dir d;
		int e = okm_fs_open_dir(at.base, ".", 1, &d);
		if (e != kal_ok) return -okm_errno(e);
		kal_u64 total = 0, avail = 0;
		e = okm_fs_capacity(d, &total, &avail);
		okm_fs_close_dir(d);
		if (e != kal_ok) return -okm_errno(e);

		for (unsigned i = 0; i < sizeof *out; i++) ((char*)out)[i] = 0;
		/* openkal reports BYTES and this record counts blocks, so a block size
		 * is chosen and the counts are derived from it rather than invented. */
		out->bsize = out->frsize = OKM_PAGE;
		out->blocks = total / OKM_PAGE;
		out->bfree = out->bavail = avail / OKM_PAGE;
		out->namelen = 255;
		return 0;
	}
#endif

#ifdef SYS_prlimit64
	case SYS_prlimit64: {
		struct okm_rlimit64 { uint64_t cur, max; };
		if ((const void*)a3) return -EPERM;         /* a new limit */
		struct okm_rlimit64* out = (void*)a4;
		if (!out) return 0;
		if ((int)a2 != RLIMIT_NOFILE) return -ENOSYS;
		out->cur = out->max = (uint64_t)OKM_MAX_FD;
		return 0;
	}
#endif

	/* ⭐ REFUSED FROM A CASE OF ITS OWN RATHER THAN FROM THE DEFAULT ARM, SO
	 * THAT THE TRACE DOES NOT REPORT IT.
	 *
	 * musl's `pthread_create' calls `__membarrier_init' the first time a
	 * context is started, and that function is one line whose RESULT IS
	 * ASSIGNED TO NOTHING --- musl's own comment says the registration is an
	 * optimisation and failing it costs nothing. Nothing in this port's musl
	 * ever calls `__membarrier' itself.
	 *
	 * ⚠️ It reached the default arm, so `OPENKAL_MUSL_TRACE=enosys' reported it
	 * beside five operations that a program actually wanted, and the first
	 * consumer to use that switch had to work out which of the six mattered.
	 * A trace whose reader must filter it is a trace that costs its reader
	 * more than it saves. */
#ifdef SYS_membarrier
	case SYS_membarrier: return -ENOSYS;
#endif
	/* SYS_umask is deliberately absent and falls to the default below.
	 *
	 * IT USED TO BE ANSWERED, AND THE ANSWER WAS A FICTION. A stored word was
	 * updated and returned, and nothing else in this port ever read it: the
	 * variable was written by this case and read by this case, and by nothing
	 * else. openkal's kal_fs_open takes no mode argument, so there is no
	 * creation for a mask to apply to.
	 *
	 * A caller therefore set a mask, was told the previous one, and observed
	 * the next file it created ignore both. That is the one outcome okm_opt.h
	 * forbids in terms: nothing here reports success having done nothing. It
	 * is a harder defect than a wrong permission bit, because a wrong bit can
	 * be argued about and an effect that does not exist cannot.
	 *
	 * Falling through means musl's umask() now reports ENOSYS. Most callers do
	 * not check it, which is correct: the success they were getting was false,
	 * and a program whose security rests upon a mask is better stopped by a
	 * visible failure than served by one that does nothing. */
	/* THE RELEASE FIELD IS THIS LIBRARY'S VERSION, AND IT USED TO BE A CONSTANT
	 * THAT WAS WRONG. It read "0.5.0" through every release after 0.5.0, so a
	 * program that asked was not left without an answer -- it was given a false
	 * one, which is the worse of the two. It now comes from the manifest, by
	 * way of build.mcpp.
	 *
	 * It therefore MOVES AT EVERY RELEASE. Nothing in this library or in musl
	 * reads it -- `gethostname` and `getdomainname` are musl's only consumers
	 * of `uname` and both read `nodename` -- but a program above it that
	 * compares this field against a fixed string will see it change, and
	 * README.md records that beside the other divergences. `sysname` is
	 * "openkal" and not "Linux", so nothing can have been reading this as a
	 * kernel version. */
	case SYS_uname: {
		struct utsname* u = (struct utsname*)a1;
		static const char* const parts[] = { "openkal", "openkal", okm_version, "openkal", 0 };
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
		/* ⚠️⚠️ THE SIZE IS THE CALLER'S, AND TAKING IT FROM THE TYPE INSTEAD
		 * DESTROYED THE CALLER'S RETURN ADDRESS.
		 *
		 * This wrote `sizeof(sigset_t)' --- 128 bytes --- into whatever `a3'
		 * named. `a4' is the SIGSETSIZE the caller declared, which is the
		 * kernel's set and is EIGHT on this architecture; the kernel writes
		 * exactly that many and no more.
		 *
		 * Sixteen of musl's seventeen callers pass a 128-byte `sigset_t' and
		 * saw nothing. The seventeenth is `src/signal/sigaction.c', which
		 * declares `unsigned long set[_NSIG/(8*sizeof(long))]' --- ONE WORD ---
		 * and reaches this only when the signal is SIGABRT. That local sits at
		 * -0x20 in a frame of 0x30, so 120 bytes past it lay the saved frame
		 * pointer and the return address, and `__sigaction' returned to zero.
		 *
		 * ⭐ MEASURED, and the whole of the reproduction is three lines:
		 *
		 *     int main(void) { signal(SIGABRT, h); return 0; }
		 *
		 * SIGSEGV with the instruction pointer at zero, the stack top zero
		 * rather than a return address, and every register but one zero ---
		 * which is a RETURN to a cleared return address and not a call through
		 * a null pointer, and reads as the second. Every other signal number
		 * returns SIG_ERR and exits 0.
		 *
		 * ⚠️ AND IT WAS NOT ONLY INSTALLING A HANDLER. `signal(SIGABRT, SIG_IGN)'
		 * and a plain enquiry, `sigaction(SIGABRT, NULL, &old)', died the same
		 * way: musl takes the lock for any change to that disposition, and
		 * blocking signals around it is how it takes it. So a program that only
		 * READ what SIGABRT was set to could not survive doing so --- which a
		 * terminal-interface library, a test framework's death tests and any
		 * crash reporter all do before they do anything else. */
		const kal_uintptr room = (kal_uintptr)a4;
		if (room > sizeof(sigset_t)) return -EINVAL;
		char* old = (char*)a3;
		if (old) for (kal_uintptr i = 0; i < room; i++) old[i] = 0;
		return 0;
	}
	case SYS_rt_sigaction: {
		/* ⚠️ THE SAME DEFECT'S OTHER HALF, IN THE OTHER DIRECTION. The
		 * old-action was cleared for the size of a structure declared HERE ---
		 * three fields --- while `struct k_sigaction' is four, so eight bytes
		 * of the caller's structure were left holding whatever the stack held
		 * and were then copied out to the program by `__libc_sigaction'. The
		 * size is taken from the caller's declared sigsetsize, as the kernel
		 * takes it, and the layout from the architecture the port is built
		 * for. */
		const struct { void* handler; unsigned long flags; void* restorer; }* act = (const void*)a2;
		if (a3) {
			/* handler, flags, restorer, and the mask whose width the caller
			 * declared in a4. */
			const kal_uintptr mask = (kal_uintptr)a4;
			if (mask > sizeof(sigset_t)) return -EINVAL;
			char* old = (char*)a3;
			for (kal_uintptr i = 0; i < sizeof *act + mask; i++) old[i] = 0;
		}
		if (!act) return 0;
		const uintptr_t h = (uintptr_t)act->handler;
		if (h == 0 || h == 1) return 0;      /* SIG_DFL and SIG_IGN */
		return -ENOSYS;
	}
#ifdef SYS_sigaltstack
	/* ⚠️ IT ANSWERED `0' AND INSTALLED NOTHING, AND THE ENQUIRY LIED TOO.
	 * Measured: install a stack, ask for it back, and the answer is a zeroed
	 * record --- reported as success, with `ss_sp' and `ss_size' both zero,
	 * rather than as "none is installed". An alternate stack is where a signal
	 * handler runs, this library delivers no signals, and so there is nothing
	 * here to install it for. Refused rather than granted. */
	case SYS_sigaltstack: return -ENOSYS;
#endif
	case SYS_rt_sigreturn: return -ENOSYS;

	default:
		/* Everything else openkal does not have. The C library above reports
		 * the failure to the program in the way the program's author already
		 * handles, which is the outcome a missing facility should produce ---
		 * and, when asked, says which one it was. */
		trace_absent(n);
		return -ENOSYS;
	}
}
