/* Every typedef this port's generated headers commit to, checked against the
 * compiler's own statement of the same type, for the target actually being
 * built.
 *
 * THE CLASS OF DEFECT THIS CATCHES, NAMED THREE TIMES BEFORE THIS FILE
 * EXISTED. `port/include/bits/setjmp.h` sized `jmp_buf` for the wrong
 * calling convention once `_WIN32` stopped answering "which register set".
 * `port/src/okm_syscall.c` passed a `uint64_t*` where `kal_fs_seek` reads a
 * `kal_u64*` --- the same width under the data model this package now
 * declares, and a different type, because `kal_u64` is `__UINT64_TYPE__`
 * and `uint64_t` is this package's own `unsigned _Int64`, and nothing
 * requires the two spellings to agree beyond the coincidence that they used
 * to. `musl-generated/aarch64-macos/bits/alltypes.h` declared `wchar_t`
 * unsigned, inherited unchanged from aarch64 Linux's AAPCS64 answer, while
 * Apple's own ABI --- the compiler's `__WCHAR_TYPE__` on that target --- says
 * signed. Three different files, three different pairs of names, one shape:
 * TWO SPELLINGS ASSUMED TO NAME THE SAME TYPE, AND NOTHING CHECKED IT.
 *
 * Each was found by a probe that happened to exercise it: a `longjmp` under
 * Wine, a file copy under Wine, a wide string literal on aarch64-macos.
 * Absent that probe, each is silent until it corrupts something. This file
 * is the check that does not need a probe: it runs at compile time, for
 * every target this package builds for, because it is an ordinary
 * translation unit in `port/src`, and the manifest already compiles every
 * one of those for every target: the glob for this directory is
 * unconditional.
 *
 * `_Generic` ASKS THE QUESTION THIS PORT ACTUALLY HAS, NOT A WEAKER ONE.
 * `sizeof(T) == sizeof(__BUILTIN_TYPE__)` would have passed all three defects
 * above --- every one of them was a same-width, different-type mismatch. A
 * `_Generic` selection matches only when the two are the same type after
 * typedefs are stripped, which is exactly the fact a pointer of one type
 * passed where the other is expected needs to hold. The `default:` arm makes
 * a mismatch a compile error naming which pair diverged, rather than a
 * silent selection of the wrong case.
 *
 * WHAT IS NOT HERE. `ssize_t`, `regoff_t`, `register_t` and their neighbours
 * are built from the same `_Addr`/`_Reg`/`_Int64` macros as the types below,
 * but the compiler has no builtin opinion about them --- they are POSIX's
 * own types, not ones a compiler predefines a canonical spelling for. There
 * is nothing to check them against, so they are not listed; the four
 * `_Addr`/`_Reg`/`_Int64`-derived types below that the compiler DOES have an
 * opinion about (`size_t`, `ptrdiff_t`, `intptr_t`, `uintptr_t`, `int64_t`,
 * `uint64_t`) already exercise the same macros. */
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#define OKM_SAME_TYPE(value, builtin_type, name)                             \
	_Static_assert(_Generic((value), builtin_type: 1, default: 0),       \
	               name " is not " #builtin_type " on this target")

OKM_SAME_TYPE((wchar_t)0,   __WCHAR_TYPE__,   "wchar_t");
OKM_SAME_TYPE((wint_t)0,    __WINT_TYPE__,    "wint_t");
OKM_SAME_TYPE((size_t)0,    __SIZE_TYPE__,    "size_t");
OKM_SAME_TYPE((ptrdiff_t)0, __PTRDIFF_TYPE__, "ptrdiff_t");
OKM_SAME_TYPE((intptr_t)0,  __INTPTR_TYPE__,  "intptr_t");
OKM_SAME_TYPE((uintptr_t)0, __UINTPTR_TYPE__, "uintptr_t");
OKM_SAME_TYPE((int32_t)0,   __INT32_TYPE__,   "int32_t");
OKM_SAME_TYPE((uint32_t)0,  __UINT32_TYPE__,  "uint32_t");
OKM_SAME_TYPE((int64_t)0,   __INT64_TYPE__,   "int64_t");
OKM_SAME_TYPE((uint64_t)0,  __UINT64_TYPE__,  "uint64_t");
OKM_SAME_TYPE((intmax_t)0,  __INTMAX_TYPE__,  "intmax_t");
OKM_SAME_TYPE((uintmax_t)0, __UINTMAX_TYPE__, "uintmax_t");

#undef OKM_SAME_TYPE
