/* A part of a file in the address space: `mmap', `munmap' and `msync' of a
 * file, over `openkal.map' (openkal 0.18).
 *
 * WEAK, LIKE EVERY OPERATION A LATER VERSION ADDED. An image linked here and
 * run by a loader whose implementation predates 0.18 binds the three names to
 * zero (SPEC 12.2), and the file mapping answers ENOSYS as it always did; a
 * program that asked for one --- LLVM's MemoryBuffer does --- reads the file
 * instead. The requirements note of such an image does not name openkal.map.
 *
 * WHAT POSIX ASKS AND OPENKAL DOES NOT STATE, KEPT HERE. openkal maps from an
 * offset that is a multiple of 64 KiB and a region that ends within the file;
 * POSIX maps from any page and beyond the end. So the offset is moved down to
 * the alignment and the address returned is inside the region, and a length
 * past the end is mapped to the end. Both are reconstructed from what the
 * caller was given, so `munmap' and `msync' must find the region again: a table
 * of the regions this library made, which is bookkeeping of what it handed out
 * and not a compatibility layer of the implementation's (clause 7.1 binds
 * implementations, and this library reconstructs POSIX).
 *
 * NO PROT_EXEC. Instructions run from memory openkal.exec published; this
 * library's `dlopen' loads through it. A request for an executable file mapping
 * is EACCES, the answer POSIX gives a mapping the system forbids. */
#include "okm.h"
#include "okm_opt.h"

#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>

#if OKM_HAS_FS
#include <openkal/map.h>

extern __typeof(kal_map_file)    kal_map_file    __attribute__((__weak__));
extern __typeof(kal_map_sync)    kal_map_sync    __attribute__((__weak__));
extern __typeof(kal_map_release) kal_map_release __attribute__((__weak__));

struct region {
	uintptr_t user;      /* what mmap returned                  */
	size_t    user_len;  /* what the caller asked for           */
	void*     base;      /* what kal_map_file wrote             */
	size_t    base_len;  /* what was passed to it               */
};

static struct region* g_regions;
static size_t g_count, g_cap;

/* Under okm_lock. Grows by doubling through kal_alloc, which is the allocator
 * beneath malloc, so a region can be recorded while malloc itself maps. */
static int record(struct region r)
{
	if (g_count == g_cap) {
		const size_t cap = g_cap ? g_cap * 2 : 64;
		struct region* n = kal_alloc(cap * sizeof *n, _Alignof(struct region));
		if (!n) return -ENOMEM;
		for (size_t i = 0; i < g_count; i++) n[i] = g_regions[i];
		if (g_regions) kal_free(g_regions, g_cap * sizeof *g_regions, _Alignof(struct region));
		g_regions = n;
		g_cap = cap;
	}
	g_regions[g_count++] = r;
	return 0;
}

static long find(uintptr_t addr)
{
	for (size_t i = 0; i < g_count; i++)
		if (addr >= g_regions[i].user && addr < g_regions[i].user + g_regions[i].user_len) return (long)i;
	return -1;
}

long okm_map_file(void* addr, size_t len, int prot, int flags, int fd, long long off)
{
	(void)addr;   /* a hint; openkal places a region where it will */
	if (!kal_map_file) return -ENOSYS;
	if (flags & MAP_FIXED) return -ENOTSUP;
	if (prot & PROT_EXEC) return -EACCES;
	if (len == 0 || off < 0) return -EINVAL;
	const int shared = (flags & MAP_SHARED) != 0;
	if (!shared && !(flags & MAP_PRIVATE)) return -EINVAL;

	struct okm_desc* d = okm_desc_of(fd);
	if (!d) return -EBADF;
	if (d->kind != OKM_FILE) return -ENODEV;

	struct kal_node_info info = { .self_size = sizeof info };
	const int ie = okm_fs_file_info(d->file, KAL_INFO_SIZE, &info);
	if (ie != kal_ok) return -okm_errno(ie);

	const kal_u64 start = (kal_u64)off & ~(KAL_MAP_ALIGNMENT - 1);
	const size_t  lead  = (size_t)((kal_u64)off - start);
	if ((kal_u64)off >= info.size) return -ENXIO;   /* nothing of the file is there */
	kal_u64 want = (kal_u64)lead + len;
	if (start + want > info.size) want = info.size - start;   /* POSIX maps past the end; openkal ends at it */

	kal_uintptr how = 0;
	if (prot & PROT_WRITE) how |= KAL_MAP_WRITE;
	if (shared)            how |= KAL_MAP_SHARED;
	void* base = 0;
	const int e = kal_map_file(d->file, start, (kal_uintptr)want, how, &base);
	if (e != kal_ok) return e == kal_err_not_supported ? -ENODEV : -okm_errno(e);

	const struct region r = { (uintptr_t)base + lead, len, base, (size_t)want };
	okm_lock();
	const int re = record(r);
	okm_unlock();
	if (re) { kal_map_release(base, (kal_uintptr)want); return re; }
	return (long)r.user;
}

int okm_map_unmap(void* addr, size_t len, long* result)
{
	(void)len;
	okm_lock();
	const long i = find((uintptr_t)addr);
	if (i < 0) { okm_unlock(); return 0; }
	const struct region r = g_regions[i];
	/* Only the whole region: one environment releases a view only whole. A
	 * caller that releases part of a file mapping is told so rather than left
	 * holding pages it believes are gone. */
	if ((uintptr_t)addr != r.user) { okm_unlock(); *result = -EINVAL; return 1; }
	g_regions[i] = g_regions[--g_count];
	okm_unlock();
	kal_map_release(r.base, (kal_uintptr)r.base_len);
	*result = 0;
	return 1;
}

int okm_map_sync(void* addr, size_t len, long* result)
{
	(void)len;
	okm_lock();
	const long i = find((uintptr_t)addr);
	const struct region r = i >= 0 ? g_regions[i] : (struct region){0};
	okm_unlock();
	if (i < 0) return 0;
	const int e = kal_map_sync(r.base, (kal_uintptr)r.base_len);
	*result = e == kal_ok ? 0 : -okm_errno(e);
	return 1;
}

#else

long okm_map_file(void* addr, size_t len, int prot, int flags, int fd, long long off)
{
	(void)addr; (void)len; (void)prot; (void)flags; (void)fd; (void)off;
	return -ENOSYS;
}
int okm_map_unmap(void* addr, size_t len, long* result) { (void)addr; (void)len; (void)result; return 0; }
int okm_map_sync(void* addr, size_t len, long* result) { (void)addr; (void)len; (void)result; return 0; }

#endif
