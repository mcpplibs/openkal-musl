/* What `long double' is, where the architecture does not decide it alone.
 *
 * musl files this header under arch/<arch>/, because for every system musl was
 * written for the architecture does decide it: on aarch64 a long double is the
 * quadruple-precision format the procedure call standard specifies, and musl's
 * arch/aarch64/bits/float.h says so.
 *
 * One of the three systems this port builds for disagrees. On that system's
 * aarch64 a long double is a double --- the compiler says LDBL_MANT_DIG 53 and
 * LDBL_MAX_EXP 1024 --- so the header musl ships describes a type the compiler
 * is not generating. Every routine that takes a long double then reads sixteen
 * bytes out of eight, and the first of them a program reaches is the one that
 * formats a floating-point number: `printf("%.3f", 1.5)' stops in `frexpl' with
 * the value read as infinity.
 *
 * Measured rather than assumed, on all four combinations this port is built
 * for:
 *
 *     target             __LDBL_MANT_DIG__   musl's arch/<arch>/bits/float.h
 *     aarch64, Apple             53                   113   <- disagrees
 *     x86_64,  Apple             64                    64
 *     x86_64,  Linux             64                    64
 *     aarch64, Linux            113                   113
 *
 * The values below are not chosen. They are the compiler's own, named through
 * the macros it defines, so that this file cannot drift from what is being
 * compiled --- which is the property the header it replaces did not have.
 * port/src/okm_float_assert.c asserts the agreement for every target, so a
 * fourth combination that disagrees is a failed build rather than a program
 * that formats a number wrongly.
 *
 * musl already supports a 53-bit long double: it is what its own arm and
 * riscv64 configurations use, and every conditional in src/math and in
 * src/stdio is written for it. Nothing beyond this header is involved.
 */
#ifndef OKM_BITS_FLOAT_H
#define OKM_BITS_FLOAT_H

#if defined(__APPLE__) && defined(__aarch64__)

#define FLT_EVAL_METHOD 0

#define LDBL_TRUE_MIN __LDBL_DENORM_MIN__
#define LDBL_MIN      __LDBL_MIN__
#define LDBL_MAX      __LDBL_MAX__
#define LDBL_EPSILON  __LDBL_EPSILON__

#define LDBL_MANT_DIG __LDBL_MANT_DIG__
#define LDBL_MIN_EXP  __LDBL_MIN_EXP__
#define LDBL_MAX_EXP  __LDBL_MAX_EXP__

#define LDBL_DIG        __LDBL_DIG__
#define LDBL_MIN_10_EXP __LDBL_MIN_10_EXP__
#define LDBL_MAX_10_EXP __LDBL_MAX_10_EXP__

#define DECIMAL_DIG __DECIMAL_DIG__

#else

#include_next <bits/float.h>

#endif

#endif
