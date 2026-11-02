/* A hosted program registers no counter: its system's own profilers measure
 * it, and no implementation beneath one provides `openkal.prof' (a loader's
 * interface, SPEC 12.3). A weak reference to it would be an undefined name the
 * Mach-O and PE links refuse. An ok image registers them
 * (port/src/image/okm_prof_image.c). */
#include "okm.h"
#include "okm_stat.h"

__attribute__((__visibility__("hidden"))) void __okm_prof_init(void) {}
