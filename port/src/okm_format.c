/* What an object format does not provide, and what it provides instead.
 *
 * musl expects four things from the object format that ELF supplies and the
 * other two formats this port builds for do not. They are collected here rather
 * than scattered, so that the cost of a format is one file rather than a search.
 *
 * The four absences are the arrays a linker builds out of the initialiser and
 * finaliser sections. musl walks them at startup and at exit. Each of the other
 * two formats runs static initialisation by a mechanism of its own, so on both
 * the four names are made to span nothing --- four labels at one address is the
 * shortest way to say that --- and what differs is only which mechanism runs
 * the initialisers and whether this file has to drive it.
 *
 *   One format builds a list with its own convention and names it differently.
 *   Nothing else drives that list when a program carries no runtime, so the
 *   loops below do. Measured, not assumed: a program linked with no runtime at
 *   all and an entry point of its own still finds the list.
 *
 *   The other format's loader runs the initialisers itself, before it transfers
 *   control to the entry point, whatever the entry point is named. There is
 *   nothing for this file to drive there, and driving it would run every
 *   initialiser twice.
 */
#if defined(_WIN32) || defined(__APPLE__)

#include "okm.h"

/* The four names musl walks between, made to span nothing. The two formats
 * differ in the directive that names a section and in whether a C name carries
 * a leading underscore, and in nothing else. */
#if defined(__APPLE__)
__asm__(
	".section __DATA,__const\n"
	".p2align 3\n"
	".globl ___init_array_start\n___init_array_start:\n"
	".globl ___init_array_end\n___init_array_end:\n"
	".globl ___fini_array_start\n___fini_array_start:\n"
	".globl ___fini_array_end\n___fini_array_end:\n"
	".text\n");
#else
__asm__(
	".section .rdata,\"dr\"\n"
	".balign 8\n"
	".globl __init_array_start\n__init_array_start:\n"
	".globl __init_array_end\n__init_array_end:\n"
	".globl __fini_array_start\n__fini_array_start:\n"
	".globl __fini_array_end\n__fini_array_end:\n"
	".text\n");
#endif

#if defined(_WIN32)

typedef void (*okm_hook)(void);

/* This format's own convention: the list begins with a sentinel of -1 and ends
 * with a null, and the entries are run in the reverse of the order they were
 * placed --- which is the order this format's own runtime uses, and therefore
 * the order a program built for it expects. */
extern okm_hook __CTOR_LIST__[];
extern okm_hook __DTOR_LIST__[];

static void run_list(okm_hook* list, int reverse)
{
	if (!list) return;
	unsigned long n = 0;
	while (list[n + 1]) ++n;
	if (reverse) { for (unsigned long i = n; i >= 1; --i) list[i](); }
	else         { for (unsigned long i = 1; i <= n; ++i) list[i](); }
}

/* musl calls these around the program. On ELF they come from the two objects a
 * C library places at the ends of the link; this format has no such objects
 * when a program carries no runtime, so they are here. */
void _init(void) { run_list(__CTOR_LIST__, 1); }
void _fini(void) { run_list(__DTOR_LIST__, 0); }

#else  /* __APPLE__ */

/* Nothing, and deliberately. This format's loader has already run every
 * initialiser in the image by the time control reaches the entry point, so a
 * loop here would run each of them a second time --- and a second run of a
 * constructor is not a slower program but a different one. */
void _init(void) { }
void _fini(void) { }

#endif

/* A placeholder that nothing replaces on either format, because the source that
 * would replace it belongs to a dynamic linker this arrangement has none of. */
void __ldso_atfork(int who) { (void)who; }

#endif  /* _WIN32 || __APPLE__ */
