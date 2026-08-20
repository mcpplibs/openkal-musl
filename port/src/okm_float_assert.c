/* That the library and the compiler agree about what a floating-point type is.
 *
 * A C library declares the properties of `float', `double' and `long double' in
 * <float.h>, and the compiler generates code for them. Nothing checks that the
 * two descriptions are of the same type, and a program that meets a
 * disagreement meets it as a wrong number rather than as a diagnostic --- the
 * one that found this printed infinity for 1.5 and then exhausted its stack.
 *
 * musl files <bits/float.h> under arch/<arch>/, which is right for every system
 * it was written for and is not right for one of the three this port builds
 * for: there a long double is a double where the architecture's own procedure
 * call standard says quadruple. port/include/bits/float.h states the
 * compiler's own answer on that combination.
 *
 * These assertions are what keeps that statement true. A fourth combination
 * that disagrees is a failed build naming the field, rather than a program
 * that formats a number wrongly.
 */
#include <float.h>

#define OKM_AGREE(name, builtin) \
	_Static_assert(name == builtin, \
	               "the C library and the compiler disagree about " #name)

OKM_AGREE(FLT_MANT_DIG,   __FLT_MANT_DIG__);
OKM_AGREE(FLT_MAX_EXP,    __FLT_MAX_EXP__);
OKM_AGREE(DBL_MANT_DIG,   __DBL_MANT_DIG__);
OKM_AGREE(DBL_MAX_EXP,    __DBL_MAX_EXP__);
OKM_AGREE(LDBL_MANT_DIG,  __LDBL_MANT_DIG__);
OKM_AGREE(LDBL_MAX_EXP,   __LDBL_MAX_EXP__);
OKM_AGREE(LDBL_MIN_EXP,   __LDBL_MIN_EXP__);
OKM_AGREE(LDBL_DIG,       __LDBL_DIG__);

/* And that the width the compiler gives the type is the width the library's
 * own union punning assumes. musl selects that union on LDBL_MANT_DIG, so the
 * two assertions together fix both the value and the storage. */
_Static_assert(sizeof(long double) * 8 >= LDBL_MANT_DIG,
               "a long double cannot hold the significand the C library declares");
