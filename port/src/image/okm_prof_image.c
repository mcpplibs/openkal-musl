/* The cells of okm_stat.h, registered where the loader measures the image
 * (openkal.prof, SPEC 12.3).
 *
 * WEAK, because a loader provides the interface to the images it binds only
 * when it measures them; where it is absent the names are null and nothing is
 * registered. */
#include "../okm.h"
#include "../okm_stat.h"
#include <openkal/prof.h>

extern __typeof(kal_prof_active)  kal_prof_active  __attribute__((__weak__));
extern __typeof(kal_prof_counter) kal_prof_counter __attribute__((__weak__));

__attribute__((__visibility__("hidden"))) void __okm_prof_init(void)
{
	if (!kal_prof_active || !kal_prof_counter || !kal_prof_active()) return;
	for (int i = 0; i < OKM_STAT_COUNT; i++) {
		kal_uintptr n = 0;
		while (__okm_stat_names[i][n]) n++;
		if (kal_prof_counter(__okm_stat_names[i], n, &__okm_stat[i]) != kal_ok) return;
	}
	__atomic_store_n(&__okm_prof_on, 1, __ATOMIC_RELEASE);
}
