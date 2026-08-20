/* The size of what setjmp records, where the environment's calling convention
 * differs from the one musl's architectures use.
 *
 * musl states this per architecture, because the set of registers a called
 * function must preserve is the processor's. It is not only the processor's:
 * two conventions exist for the same processor here, and the one this
 * environment uses adds four general registers and ten vector registers to the
 * set. A record sized for the other convention would be overrun by the sequence
 * that fills it.
 *
 * Everywhere else this header defers to musl's own.
 */
#ifndef OKM_BITS_SETJMP_H
#define OKM_BITS_SETJMP_H

#if defined(_WIN32) && defined(__x86_64__)
/* eight general registers, the stack pointer, the resumption address, and ten
 * vector registers of sixteen bytes each: thirty machine words, rounded up. */
typedef unsigned long long __jmp_buf[32];
#else
#include_next <bits/setjmp.h>
#endif

#endif
