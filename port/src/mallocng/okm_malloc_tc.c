/* A THREAD'S SMALL ALLOCATIONS FROM ITS OWN CACHE (0.25.0).
 *
 * mallocng takes its one lock for every allocation (`rdlock' is exclusive in
 * this configuration), so allocating threads run one at a time. Measured with
 * an ok image on Linux x86_64 (release build), a malloc and free of 16 to 528
 * bytes cost 38.5 ns on one thread, 194 ns on each of two and 521 ns on each
 * of four; clangd.okb, whose workers allocate together, ran on less than one of
 * four processors while the native clangd used three. With this cache: 31, 41
 * and 61 ns.
 *
 * Once a program has a second thread, each thread keeps the slots it frees of
 * the small size classes and takes them back without the lock; when it has
 * none, the one locked call that serves it also claims a few more of the same
 * class. A slot in a cache is one mallocng counts as in use: its group's masks
 * do not have it, so the group is never released beneath the cache, and its
 * bytes are exactly the ones mallocng's own free leaves --- free invalidates
 * the header before the slot is kept --- so taking it back is mallocng's own
 * reuse of a freed slot, by `enframe'. A thread's cache goes back to mallocng
 * when the thread ends (__pthread_exit), and is bounded in between: at most
 * TC_MAX slots of each of TC_CLASSES classes, the largest 2032 bytes.
 *
 * A program with one thread is served exactly as before, and so is every
 * allocation of a larger class. musl/PATCHES.md records the lines this needs in
 * mallocng and in the thread's record. */
#include "../../../musl/src/malloc/mallocng/meta.h"
#include "pthread_impl.h"

hidden void __malloc_release_slot(struct meta *, int);

#define TC_CLASSES 24   /* size classes 0..23: slots of at most 127 units */
#define TC_MAX     16   /* slots kept per class */
#define TC_BATCH    8   /* slots one locked call claims for the cache */

struct malloc_tc {
	unsigned char n[TC_CLASSES];
	unsigned char idx[TC_CLASSES][TC_MAX];
	struct meta *g[TC_CLASSES][TC_MAX];
};

/* The record's field: 0 before the thread's first use, these two while the
 * cache is being mapped or after the thread gave it back, else the cache. */
#define TC_PENDING ((void *)1)
#define TC_OFF     ((void *)2)

static struct malloc_tc *held(void *tc)
{
	return (tc == 0 || tc == TC_PENDING || tc == TC_OFF) ? 0 : tc;
}

/* The calling thread's cache, mapped at its first use. Never under the lock:
 * the mapping is a call this library makes into openkal, and a recursive
 * allocation meanwhile finds the field pending and is served by mallocng. */
static struct malloc_tc *own_cache(void)
{
	pthread_t self = __pthread_self();
	void *tc = self->malloc_tc;
	if (tc) return held(tc);
	self->malloc_tc = TC_PENDING;
	void *m = mmap(0, sizeof(struct malloc_tc), PROT_READ|PROT_WRITE,
		MAP_PRIVATE|MAP_ANON, -1, 0);
	self->malloc_tc = m == MAP_FAILED ? TC_OFF : m;
	return m == MAP_FAILED ? 0 : m;
}

/* A slot of `n' bytes from the cache, framed as mallocng frames a freed slot
 * it reuses; 0 when the cache has none of the class. A slot of the coarse
 * class above an even one is larger than asked and serves it too, as mallocng's
 * own choice of a coarse class does. */
hidden void *__malloc_tc_take(size_t n, int sc)
{
	if (sc >= TC_CLASSES) return 0;
	struct malloc_tc *tc = own_cache();
	if (!tc) return 0;
	int c = sc;
	if (!tc->n[c]) {
		if (sc < 4 || (sc & 1) || (sc|1) >= TC_CLASSES || !tc->n[sc|1]) return 0;
		c = sc|1;
	}
	int k = --tc->n[c];
	return enframe(tc->g[c][k], tc->idx[c][k], n, ctx.mmap_counter);
}

/* Under the lock, after the call that served this thread: a few more slots of
 * the class it asked for, from the class's active group. */
hidden void __malloc_tc_fill(int sc)
{
	if (sc >= TC_CLASSES) return;
	struct malloc_tc *tc = held(__pthread_self()->malloc_tc);
	if (!tc) return;
	struct meta *g = ctx.active[sc];
	while (g && tc->n[sc] < TC_BATCH) {
		uint32_t mask = g->avail_mask;
		uint32_t first = mask & -mask;
		if (!first) break;
		g->avail_mask = mask - first;
		int k = tc->n[sc]++;
		tc->g[sc][k] = g;
		tc->idx[sc][k] = a_ctz_32(first);
	}
}

/* A slot free has validated and invalidated, kept by the calling thread; 0
 * when it is mallocng's to take back --- a class the cache does not hold, a
 * group of one slot, a full class, or a thread without a cache. */
hidden int __malloc_tc_give(struct meta *g, int idx)
{
	int sc = g->sizeclass;
	if (sc >= TC_CLASSES || !g->last_idx) return 0;
	struct malloc_tc *tc = held(__pthread_self()->malloc_tc);
	if (!tc || tc->n[sc] >= TC_MAX) return 0;
	int k = tc->n[sc]++;
	tc->g[sc][k] = g;
	tc->idx[sc][k] = idx;
	return 1;
}

/* The ending thread's slots back to mallocng, before the thread list's lock
 * is taken: fork takes the allocator's lock first and that one second. What
 * the thread frees afterwards goes to mallocng directly. */
hidden void __malloc_tc_flush(void)
{
	pthread_t self = __pthread_self();
	struct malloc_tc *tc = held(self->malloc_tc);
	self->malloc_tc = TC_OFF;
	if (!tc) return;
	for (int c = 0; c < TC_CLASSES; c++)
		while (tc->n[c]) {
			int k = --tc->n[c];
			__malloc_release_slot(tc->g[c][k], tc->idx[c][k]);
		}
	munmap(tc, sizeof *tc);
}
