/* What this object format does not provide, and what it provides instead.
 *
 * musl expects four things from the object format that ELF supplies and this
 * one does not, and one thing that this one supplies under a different name.
 * They are collected here rather than scattered, so that the cost of the
 * format is one file rather than a search.
 *
 * The four absences are the arrays a linker builds out of the initialiser and
 * finaliser sections. musl walks them at startup and at exit. This format
 * builds a list instead, with its own convention, and names it differently.
 *
 * The one presence is that list: __CTOR_LIST__ and __DTOR_LIST__ are provided
 * by the default linker script --- measured, not assumed: a program linked with
 * no runtime at all and an entry point of its own still finds them and still
 * runs its constructors. So static initialisation works here, and the loops
 * below are what makes it work.
 */
#if defined(_WIN32)

#include "okm.h"

/* The four names musl walks between. Each pair is made to span nothing, so the
 * loops that walk them run zero times and the work is done by the loops below
 * instead. Four labels at one address is the shortest way to say that. */
__asm__(
	".section .rdata,\"dr\"\n"
	".balign 8\n"
	".globl __init_array_start\n__init_array_start:\n"
	".globl __init_array_end\n__init_array_end:\n"
	".globl __fini_array_start\n__fini_array_start:\n"
	".globl __fini_array_end\n__fini_array_end:\n"
	".text\n");

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

/* A placeholder that nothing replaces on this format, because the source that
 * would replace it belongs to a dynamic linker this arrangement has none of. */
void __ldso_atfork(int who) { (void)who; }

#endif  /* _WIN32 */
