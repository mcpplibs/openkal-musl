/* musl's <sys/random.h>, plus the declaration a consumer expects to find here.
 *
 * ⭐ THE SAME QUESTION EVERY OTHER OVERLAY IN THIS ECOSYSTEM ANSWERS: consumers
 * ask "which OPERATING SYSTEM is this" and assume that system's C library. Here
 * the C library is musl, whatever the system underneath.
 *
 * `getentropy` and `getrandom` live in different headers depending on the C
 * library. glibc declares both here; musl declares `getrandom` here and
 * `getentropy` in <unistd.h>. A consumer written against glibc's layout
 * includes this header alone and does not find `getentropy`. Measured
 * 2026-08-25, building libc++'s `src/random.cpp` with
 * `_LIBCPP_USING_GETENTROPY`:
 *
 *     random.cpp:52:14: error: use of undeclared identifier 'getentropy'
 *
 * ⚠️ THIS ADDS A DECLARATION AND NOTHING ELSE. The definition is musl's, in
 * `src/misc/getentropy.c`, unchanged; what this file corrects is where a
 * consumer looks for its name. Declaring it in both places is what glibc does
 * and what the consumer was written against.
 */
#ifndef OKM_SYS_RANDOM_H
#define OKM_SYS_RANDOM_H

#include_next <sys/random.h>

#define __NEED_size_t
#include <bits/alltypes.h>

#ifdef __cplusplus
extern "C" {
#endif

int getentropy(void*, size_t);

#ifdef __cplusplus
}
#endif

#endif /* OKM_SYS_RANDOM_H */
