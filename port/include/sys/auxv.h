/* musl's <sys/auxv.h>, plus FreeBSD's way to read the auxiliary vector.
 *
 * THE SAME QUESTION EVERY OTHER OVERLAY IN THIS ECOSYSTEM ANSWERS: a consumer
 * asks "which OPERATING SYSTEM is this" and assumes that system's C library.
 * On `<arch>-freebsd-musl' the system is FreeBSD and the C library is musl, and
 * a consumer written for FreeBSD reads the auxiliary vector with
 * `elf_aux_info' (FreeBSD 12, <sys/auxv.h>) where musl offers `getauxval'.
 * Measured 2026-11-10, building compiler-rt's aarch64 `cpu_model' for
 * `aarch64-freebsd-musl' (openkal-llvm-runtime):
 *
 *     elf_aux_info.inc:3:16: error: call to undeclared function 'elf_aux_info'
 *
 * THE DEFINITION IS port/src/okm_elf_aux_info.c, over musl's own vector: this
 * port's start states the entries it has (okm_start.c), in musl's numbering,
 * which is the numbering of the keys a consumer passes. An entry the vector
 * does not hold --- AT_HWCAP, among others --- answers ENOENT, as FreeBSD's
 * does for an entry its kernel did not give.
 */
#include_next <sys/auxv.h>

#ifndef OKM_SYS_AUXV_H
#define OKM_SYS_AUXV_H

#if defined(__FreeBSD__)
#ifdef __cplusplus
extern "C" {
#endif
int elf_aux_info(int, void *, int);
#ifdef __cplusplus
}
#endif
#endif

#endif
