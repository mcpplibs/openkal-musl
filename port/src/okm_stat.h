/* What this library counts about its own work, for a measurement that asks
 * (openkal.prof, 0.26.0).
 *
 * A loader that binds an image's names counts the calls into openkal and
 * samples where the program is; it does not see why a call was made --- that an
 * allocation went to the environment for a region, that a context waited for a
 * lock rather than worked. These cells say so. Each is registered with
 * `kal_prof_counter' once, when the library starts, and only when a
 * measurement records the program (`kal_prof_active'); otherwise nothing is
 * registered and every update below is one load of a flag that is zero.
 *
 * WRITTEN AGAINST NOTHING OF musl's, because the allocator's primitive layer
 * (port/src/mimalloc) updates them and is compiled against the public headers
 * alone. */
#ifndef OKM_STAT_H
#define OKM_STAT_H

#include <openkal/types.h>

enum okm_stat {
	OKM_STAT_MALLOC_CALLS,      /* allocations requested of the allocator     */
	OKM_STAT_MALLOC_BYTES,      /* bytes they asked for                       */
	OKM_STAT_FREE_CALLS,        /* releases                                   */
	OKM_STAT_REGION_RESERVES,   /* ranges reserved of the environment         */
	OKM_STAT_REGION_RELEASES,   /* ranges given back whole                    */
	OKM_STAT_REGION_RESERVED,   /* bytes reserved now (a level, not a count)  */
	OKM_STAT_REGION_COMMITS,    /* parts backed after reservation             */
	OKM_STAT_REGION_DECOMMITS,  /* parts whose backing was returned           */
	OKM_STAT_REGION_DISCARDS,   /* parts whose contents were let go           */
	OKM_STAT_FUTEX_WAITS,       /* waits of a lock or a condition             */
	OKM_STAT_FD_LOCK_WAITS,     /* waits for the descriptor table             */
	OKM_STAT_COUNT
};

__attribute__((__visibility__("hidden"))) extern int __okm_prof_on;
__attribute__((__visibility__("hidden"))) extern volatile kal_u64 __okm_stat[OKM_STAT_COUNT];

/* Registers the cells where a measurement records this program. */
__attribute__((__visibility__("hidden"))) void __okm_prof_init(void);

static inline void okm_stat_add(enum okm_stat i, kal_u64 n)
{
	if (__builtin_expect(__okm_prof_on, 0))
		__atomic_fetch_add(&__okm_stat[i], n, __ATOMIC_RELAXED);
}

static inline void okm_stat_sub(enum okm_stat i, kal_u64 n)
{
	if (__builtin_expect(__okm_prof_on, 0))
		__atomic_fetch_sub(&__okm_stat[i], n, __ATOMIC_RELAXED);
}

#endif
