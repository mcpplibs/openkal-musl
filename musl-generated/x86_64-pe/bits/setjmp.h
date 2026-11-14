/* The record setjmp writes on a PE target, x86_64: the Microsoft x64 calling
 * convention's callee-saved state.
 *
 * SELECTED BY THE MANIFEST, NOT BY A MACRO. This directory is on the include
 * line of the x86_64 PE rows only (`cfg(all(arch = "x86_64", object-format =
 * "pe"))` in mcpp.toml), ahead of musl/arch/x86_64, so a consumer's compile
 * reads this record on those rows and musl's own on every other. Windows and
 * UEFI are both such rows; port/src/okm_setjmp.S, which writes the record, is
 * selected by the same container (OKM_TARGET_PE). */
#ifndef OKM_BITS_SETJMP_H
#define OKM_BITS_SETJMP_H

/* eight general registers, the stack pointer, the resumption address, and ten
 * vector registers of sixteen bytes each: thirty machine words, rounded up. */
typedef unsigned long long __jmp_buf[32];

#endif
