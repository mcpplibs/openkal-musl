/* Anonymous memory over `openkal.region' (openkal 0.20; this library 0.26.0).
 *
 * WHAT IT REPLACES. An anonymous `mmap' was `kal_alloc', which promises an
 * alignment and a size and not contents, and this layer then wrote a zero to
 * every byte so that the mapping read as zero, as POSIX promises. `munmap' was
 * `kal_free', and `madvise' was accepted and did nothing. So every mapping
 * cost a write to each of its pages before the program had touched one, and
 * memory a program had finished with --- an allocator's freed pages --- stayed
 * resident until the whole mapping went. Measured in clangd.okb on Linux
 * x86_64: 792 thousand page faults in a warm start, against 7.7 thousand for
 * the native program.
 *
 * WHAT IT IS. A mapping is a range reserved and backed at once
 * (`KAL_REGION_COMMIT'), which reads as zero because a range first backed does;
 * `munmap' of the whole of one releases it; `MADV_FREE' is `kal_region_discard'
 * and `MADV_DONTNEED' returns the backing and backs the part again, after which
 * it reads as zero, as POSIX's `MADV_DONTNEED' of private memory does.
 *
 * A PART OF A MAPPING IS NOT RELEASED, because one environment releases a
 * reservation only whole (openkal's region.h). `munmap' of a part returns that
 * part's backing and keeps its addresses reserved, until the whole is unmapped
 * by the address and length it was mapped with. A program that unmaps a mapping
 * piece by piece keeps the address range, not the memory.
 *
 * THE RANGES ARE RECORDED, because POSIX's calls name an address and a length
 * and the region's release needs the reservation's own; the record is ordered
 * by address, so a part is found by a search. It grows through `kal_alloc',
 * which is beneath the allocator, so a range can be recorded while the
 * allocator itself maps. Its own lock: a mapping is not a descriptor.
 *
 * WEAK. An implementation without the interface --- a machine with no page
 * table --- does not define the names, and `okm_region_map' answers ENOSYS, on
 * which the caller obtains memory as before. */
#define _GNU_SOURCE
#include "okm.h"
#include "okm_opt.h"
#include "okm_stat.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

extern __typeof(kal_region_reserve)  kal_region_reserve  __attribute__((__weak__));
extern __typeof(kal_region_commit)   kal_region_commit   __attribute__((__weak__));
extern __typeof(kal_region_decommit) kal_region_decommit __attribute__((__weak__));
extern __typeof(kal_region_discard)  kal_region_discard  __attribute__((__weak__));
extern __typeof(kal_region_release)  kal_region_release  __attribute__((__weak__));

struct range { uintptr_t base; size_t size; };

static struct range* g_ranges;
static size_t g_count, g_cap;
static volatile kal_u32 g_lock;

static int have(void)
{
	return kal_region_reserve && kal_region_commit && kal_region_decommit
	    && kal_region_discard && kal_region_release;
}

/* What this library reports as its page (okm_start.c): openkal's granularity,
 * no smaller than 4096 and a power of two. A part a program names in pages is
 * therefore a part in the region's granularity. */
static size_t grain(void)
{
	kal_uintptr g = kal_memory_granularity();
	if (g < 4096) return 4096;
	if (g & (g - 1)) {
		kal_uintptr p = 4096;
		while (p && p < g) p <<= 1;
		return p ? p : 4096;
	}
	return g;
}

/* The first range whose end is above `addr', under the lock. */
static size_t lower(uintptr_t addr)
{
	size_t lo = 0, hi = g_count;
	while (lo < hi) {
		const size_t mid = lo + (hi - lo) / 2;
		if (g_ranges[mid].base + g_ranges[mid].size <= addr) lo = mid + 1;
		else hi = mid;
	}
	return lo;
}

static long find(uintptr_t addr)
{
	const size_t i = lower(addr);
	if (i < g_count && g_ranges[i].base <= addr) return (long)i;
	return -1;
}

static int record(struct range r)
{
	if (g_count == g_cap) {
		const size_t cap = g_cap ? g_cap * 2 : 256;
		struct range* n = kal_alloc(cap * sizeof *n, _Alignof(struct range));
		if (!n) return -ENOMEM;
		if (g_count) memcpy(n, g_ranges, g_count * sizeof *n);
		if (g_ranges) kal_free(g_ranges, g_cap * sizeof *g_ranges, _Alignof(struct range));
		g_ranges = n;
		g_cap = cap;
	}
	const size_t i = lower(r.base);
	memmove(&g_ranges[i + 1], &g_ranges[i], (g_count - i) * sizeof *g_ranges);
	g_ranges[i] = r;
	g_count++;
	return 0;
}

long okm_region_map(size_t len)
{
	if (!have()) return -ENOSYS;
	const size_t g = grain();
	if (len == 0) return -EINVAL;
	if (len > (size_t)-1 - (g - 1)) return -ENOMEM;
	const size_t size = (len + g - 1) & ~(g - 1);
	void* p = kal_region_reserve(size, g, KAL_REGION_COMMIT);
	if (!p) return -ENOMEM;
	okm_mutex_lock(&g_lock, OKM_STAT_FUTEX_WAITS);
	const int e = record((struct range){ (uintptr_t)p, size });
	okm_mutex_unlock(&g_lock);
	if (e) { kal_region_release(p, size); return e; }
	okm_stat_add(OKM_STAT_REGION_RESERVES, 1);
	okm_stat_add(OKM_STAT_REGION_RESERVED, size);
	return (long)(uintptr_t)p;
}

/* The part of [addr, addr+len) that lies in range `r', in whole pages. */
static int part_of(struct range r, uintptr_t addr, size_t len, size_t g,
                   uintptr_t* lo, uintptr_t* hi)
{
	uintptr_t a = addr, b = addr + ((len + g - 1) & ~(g - 1));
	if (a < r.base) a = r.base;
	if (b > r.base + r.size) b = r.base + r.size;
	*lo = a; *hi = b;
	return a < b;
}

int okm_region_unmap(void* addr, size_t len, long* result)
{
	if (!have()) return 0;
	const size_t g = grain();
	const uintptr_t a = (uintptr_t)addr;
	okm_mutex_lock(&g_lock, OKM_STAT_FUTEX_WAITS);
	const long i = find(a);
	if (i < 0) { okm_mutex_unlock(&g_lock); return 0; }
	const struct range r = g_ranges[i];
	if (a & (g - 1)) { okm_mutex_unlock(&g_lock); *result = -EINVAL; return 1; }
	uintptr_t lo, hi;
	part_of(r, a, len, g, &lo, &hi);
	if (lo == r.base && hi == r.base + r.size) {
		memmove(&g_ranges[i], &g_ranges[i + 1], (g_count - (size_t)i - 1) * sizeof *g_ranges);
		g_count--;
		okm_mutex_unlock(&g_lock);
		kal_region_release((void*)r.base, r.size);
		okm_stat_add(OKM_STAT_REGION_RELEASES, 1);
		okm_stat_sub(OKM_STAT_REGION_RESERVED, r.size);
		*result = 0;
		return 1;
	}
	okm_mutex_unlock(&g_lock);
	/* A part: its backing goes, its addresses stay reserved. */
	if (lo < hi) {
		kal_region_decommit((void*)lo, hi - lo);
		okm_stat_add(OKM_STAT_REGION_DECOMMITS, 1);
	}
	*result = 0;
	return 1;
}

int okm_region_advise(void* addr, size_t len, int advice, long* result)
{
	if (!have()) return 0;
	if (advice != MADV_DONTNEED && advice != MADV_FREE) return 0;
	const size_t g = grain();
	const uintptr_t a = (uintptr_t)addr;
	okm_mutex_lock(&g_lock, OKM_STAT_FUTEX_WAITS);
	const long i = find(a);
	const struct range r = i >= 0 ? g_ranges[i] : (struct range){ 0, 0 };
	okm_mutex_unlock(&g_lock);
	if (i < 0) return 0;
	if (a & (g - 1)) { *result = -EINVAL; return 1; }
	uintptr_t lo, hi;
	if (!part_of(r, a, len, g, &lo, &hi)) { *result = 0; return 1; }
	int e;
	if (advice == MADV_FREE) {
		e = kal_region_discard((void*)lo, hi - lo);
		okm_stat_add(OKM_STAT_REGION_DISCARDS, 1);
	}
	else {
		/* The part reads as zero afterwards and may be used at once. */
		e = kal_region_decommit((void*)lo, hi - lo);
		if (e == kal_ok) e = kal_region_commit((void*)lo, hi - lo);
		okm_stat_add(OKM_STAT_REGION_DECOMMITS, 1);
		okm_stat_add(OKM_STAT_REGION_COMMITS, 1);
	}
	*result = e == kal_ok ? 0 : -okm_errno(e);
	return 1;
}
