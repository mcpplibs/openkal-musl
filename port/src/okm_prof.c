/* The cells of okm_stat.h and their names. They are registered where a
 * measurement records the program: in an ok image, whose loader provides
 * `openkal.prof' (port/src/image/okm_prof_image.c, openkal SPEC 12.3). A hosted
 * program is measured by its system's own profilers, and no implementation
 * beneath one provides the interface (port/src/okm_prof_hosted.c). */
#include "okm.h"
#include "okm_stat.h"

__attribute__((__visibility__("hidden"))) int __okm_prof_on;
__attribute__((__visibility__("hidden"))) volatile kal_u64 __okm_stat[OKM_STAT_COUNT];

__attribute__((__visibility__("hidden"))) const char* const __okm_stat_names[OKM_STAT_COUNT] = {
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
