/* `__tls_get_addr': the address of a thread-local variable that a program asks
 * for at run time (the general-dynamic model), in place of musl's own,
 * src/thread/__tls_get_addr.c.
 *
 * musl's answers from the vector in front of its own descriptor, and here that
 * vector is empty: the program's thread-local storage is the toolchain's and the
 * kernel layer's, not this layer's (okm_start.c, `__copy_tls'). Its answer was
 * the offset alone --- measured on riscv64 Linux under qemu as an address just
 * below zero, where the first general-dynamic access of the specification's
 * conformance suite faulted.
 *
 * A static program is one module, and its block is where the local-exec model
 * finds it from the thread pointer the kernel layer installs, the same block
 * every context's accesses of the other models reach. On riscv64 the block
 * begins at the thread pointer (variant I, with no control block before it),
 * and the offset the linker writes for the module is biased by DTP_OFFSET
 * (0x800; the port's pthread_arch.h states it, as musl's own header does).
 *
 * On x86_64, i386 and aarch64 the static link rewrites every general-dynamic
 * access into the local-exec form, and the rows of i386 that cannot set the
 * thread pointer compile with emulated thread-local storage, so no access
 * reaches this function there. One that does is refused aloud, not answered. */
#include "okm.h"
#include "pthread_impl.h"

void *__tls_get_addr(tls_mod_off_t *v)
{
#if defined(__riscv)
	char *tp;
	__asm__ ("mv %0, tp" : "=r"(tp));
	return tp + v[1] + DTP_OFFSET;
#else
	(void)v;
	static const char m[] = "openkal-musl: a general-dynamic thread-local access the static link did not resolve\n";
	kal_abort(m, sizeof m - 1);
#endif
}
