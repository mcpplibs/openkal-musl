/* The cells of okm_stat.h, registered where a measurement records the program.
 *
 * WEAK, because `openkal.prof' is provided by a loader to the images it binds
 * and by an implementation only if it chooses (openkal SPEC 12.3); where it is
 * absent the names are null and nothing is registered. */
#include "okm.h"
#include "okm_stat.h"
#include <openkal/prof.h>

extern __typeof(kal_prof_active)  kal_prof_active  __attribute__((__weak__));
extern __typeof(kal_prof_counter) kal_prof_counter __attribute__((__weak__));

__attribute__((__visibility__("hidden"))) int __okm_prof_on;
__attribute__((__visibility__("hidden"))) volatile kal_u64 __okm_stat[OKM_STAT_COUNT];

static const char* const g_names[OKM_STAT_COUNT] = {
	[OKM_STAT_MALLOC_CALLS]     = "libc.malloc.calls",
	[OKM_STAT_MALLOC_BYTES]     = "libc.malloc.bytes",
	[OKM_STAT_FREE_CALLS]       = "libc.free.calls",
	[OKM_STAT_REGION_RESERVES]  = "libc.region.reserves",
	[OKM_STAT_REGION_RELEASES]  = "libc.region.releases",
	[OKM_STAT_REGION_RESERVED]  = "libc.region.reserved_bytes",
	[OKM_STAT_REGION_COMMITS]   = "libc.region.commits",
	[OKM_STAT_REGION_DECOMMITS] = "libc.region.decommits",
	[OKM_STAT_REGION_DISCARDS]  = "libc.region.discards",
	[OKM_STAT_FUTEX_WAITS]      = "libc.futex.waits",
	[OKM_STAT_FD_LOCK_WAITS]    = "libc.fd.lock_waits",
};

__attribute__((__visibility__("hidden"))) void __okm_prof_init(void)
{
	if (!kal_prof_active || !kal_prof_counter || !kal_prof_active()) return;
	for (int i = 0; i < OKM_STAT_COUNT; i++) {
		kal_uintptr n = 0;
		while (g_names[i][n]) n++;
		if (kal_prof_counter(g_names[i], n, &__okm_stat[i]) != kal_ok) return;
	}
	__atomic_store_n(&__okm_prof_on, 1, __ATOMIC_RELEASE);
}
