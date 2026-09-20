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
 *
 * THIS HEADER IS INSTALLED, AND THAT RULES OUT THE ANSWER port/src USES. A
 * program above this package is not built with OKM_MUSL_INTERNAL or with
 * OKM_TARGET_WINDOWS --- those are this package's own private build defines
 * (mcpp.toml, [build] and [target.'cfg(windows)'.build]) and do not reach a
 * consumer's compile command, on purpose. Reading either here would size
 * `jmp_buf` one way when this library's own `setjmp.c` is built and another
 * way when a program that calls it is, which is worse than the defect this
 * file exists to fix: a mismatch nothing reports until the record overruns.
 *
 * `_WIN32` answered the question correctly right up until `[c-abi] presents =
 * "posix"` (mcpp.toml) stopped defining it on this target, for every
 * translation unit built for it, a program's included. `__CYGWIN__` is what
 * is defined in its place, target-wide rather than package-wide: mcpp keeps
 * it defined for exactly this environment --- PE object format, POSIX C
 * environment --- which is the one question this file needs answered, and
 * `__x86_64__` beside it is unaffected by any of this, being the compiler's
 * own statement of the architecture. A generated header written for this
 * package's own build was the other candidate and was not chosen: it would
 * state the target from a place only this package controls, for a fact the
 * toolchain already states the same way for every compile of this target,
 * this file's own included.
 *
 * `port/src/okm_setjmp.S` IS THE OTHER HALF OF THIS FACT --- what actually
 * writes the record this header sizes --- and it reads `OKM_TARGET_WINDOWS`
 * rather than `__CYGWIN__`, because it is not installed: it is compiled only
 * by this package's own build, so the private define reaches it, and
 * measured, mcpp's `[c-abi]` realization (the substitution that defines
 * `__CYGWIN__`) does not reach a `.S` compile the way it reaches this header's
 * readers. The two files do not read the same macro; they answer the same
 * question the same way for the same target, which is what has to hold. A
 * reader changing one of the two must check the other agrees. */
#ifndef OKM_BITS_SETJMP_H
#define OKM_BITS_SETJMP_H

/* `__MCPP_TARGET_WINDOWS__` FIRST, `__CYGWIN__` STILL ACCEPTED.
 *
 * The question this file asks is "is the target Windows",
 * because that decides the calling convention and so the size of the record
 * below. `__CYGWIN__` answered it only by accident: mcpp kept the name
 * defined because nothing else named the target, and upstream code reads it
 * as "Win32 is available" --- a 30-member measurement found four packages
 * doing exactly that and reaching `#include <windows.h>`. A BORROWED NAME
 * MEANS WHAT THE LENDER'S HISTORY MADE IT MEAN.
 *
 * mcpp now states the fact itself, for every target and under its own name
 * (`docs/21`, "The macros mcpp defines").
 *
 * TWO OPERANDS COVER EVERY ENGINE, AND THE ORDER OF RELEASES DOES NOT MATTER.
 * An engine up to and including 2026.9.21.1 defines `__CYGWIN__`, which
 * answers; the release that withdraws it defines `__MCPP_TARGET_WINDOWS__`,
 * which answers instead. There is no engine that defines neither, so this
 * header has no flag day and the two releases may land in either order.
 *
 * 2026.9.21.1 spelt the name in lower case. It is not read here and does not
 * need to be: that spelling existed for one release, nothing consumed it, and
 * the release that withdraws `__CYGWIN__` renames it in the same change ---
 * project-owned macros are upper case, as `NDEBUG` and every other are, while
 * lower case belongs to the compiler's own predefines (`__linux__`), which
 * mcpp supplies but does not own. The second operand is not redundancy; it is
 * what removes the ordering constraint, and it goes once the withdrawal has
 * shipped. */
#if (defined(__MCPP_TARGET_WINDOWS__) || defined(__CYGWIN__)) \
    && defined(__x86_64__)
/* eight general registers, the stack pointer, the resumption address, and ten
 * vector registers of sixteen bytes each: thirty machine words, rounded up. */
typedef unsigned long long __jmp_buf[32];
#else
#include_next <bits/setjmp.h>
#endif

#endif
