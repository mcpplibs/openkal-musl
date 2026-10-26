/* Every name of the specification, weakly, by name: what a placed image's
 * `kal_*' imports are bound to (okm_image.c's dlopen).
 *
 * Weak, so that an image built on this library requires no interface by
 * holding the table (SPEC 12.4: a weak import is optional and enters no
 * requirement), and a name the implementation does not provide is zero and is
 * reported by the loader as missing. The list is openkal's SURFACE.txt, read by
 * build.mcpp into `okm_kal_names.inc'; nothing else here includes a header, so
 * the declarations below cannot disagree with the specification's. */
#include <string.h>

#define OKM_KAL(n) extern void n(void) __attribute__((__weak__));
#include "okm_kal_names.inc"
#undef OKM_KAL

static const struct { const char *name; void (*fn)(void); } table[] = {
#define OKM_KAL(n) { #n, n },
#include "okm_kal_names.inc"
#undef OKM_KAL
};

void *__okm_kal_resolve(const char *name)
{
	for (unsigned i = 0; i < sizeof table / sizeof table[0]; i++)
		if (strcmp(table[i].name, name) == 0) return (void *)table[i].fn;
	return 0;
}
