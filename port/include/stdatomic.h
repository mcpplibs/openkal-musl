/* <stdatomic.h>, for C (0.26.0).
 *
 * WHY THIS LIBRARY SUPPLIES IT. This package is compiled with `-nostdinc' and
 * publishes that flag to the programs above it (mcpp.toml): the headers a
 * program sees are this C library's and no other. A compiler ships a handful of
 * headers of its own that a C library does not --- and musl supplies its own
 * version of every one of them except this, which upstream musl leaves to the
 * compiler. Under `-nostdinc' the compiler's copy is not on the path either, so
 * a C11 program that included it was told the file does not exist. The first
 * to find out was this library itself, when it began to embed an allocator
 * written in C11 (`malloc-mimalloc').
 *
 * WHAT IT IS. The C11 interface (clause 7.17) over the compilers' own
 * operations: Clang's `__c11_atomic_*', which take `_Atomic' objects, and GCC's
 * `__atomic_*', which take any object; the two compilers' own headers are built
 * the same way. Nothing here is a function, so nothing is added to the library.
 *
 * C ONLY. A C++ program reaches atomics through <atomic>, and a standard C++
 * library that offers <stdatomic.h> to C++ supplies its own. */
#ifndef _STDATOMIC_H
#define _STDATOMIC_H

#ifndef __cplusplus

#include <stddef.h>
#include <stdint.h>

typedef enum memory_order {
	memory_order_relaxed = __ATOMIC_RELAXED,
	memory_order_consume = __ATOMIC_CONSUME,
	memory_order_acquire = __ATOMIC_ACQUIRE,
	memory_order_release = __ATOMIC_RELEASE,
	memory_order_acq_rel = __ATOMIC_ACQ_REL,
	memory_order_seq_cst = __ATOMIC_SEQ_CST
} memory_order;

#define ATOMIC_BOOL_LOCK_FREE     __GCC_ATOMIC_BOOL_LOCK_FREE
#define ATOMIC_CHAR_LOCK_FREE     __GCC_ATOMIC_CHAR_LOCK_FREE
#define ATOMIC_CHAR16_T_LOCK_FREE __GCC_ATOMIC_CHAR16_T_LOCK_FREE
#define ATOMIC_CHAR32_T_LOCK_FREE __GCC_ATOMIC_CHAR32_T_LOCK_FREE
#define ATOMIC_WCHAR_T_LOCK_FREE  __GCC_ATOMIC_WCHAR_T_LOCK_FREE
#define ATOMIC_SHORT_LOCK_FREE    __GCC_ATOMIC_SHORT_LOCK_FREE
#define ATOMIC_INT_LOCK_FREE      __GCC_ATOMIC_INT_LOCK_FREE
#define ATOMIC_LONG_LOCK_FREE     __GCC_ATOMIC_LONG_LOCK_FREE
#define ATOMIC_LLONG_LOCK_FREE    __GCC_ATOMIC_LLONG_LOCK_FREE
#define ATOMIC_POINTER_LOCK_FREE  __GCC_ATOMIC_POINTER_LOCK_FREE

#define ATOMIC_VAR_INIT(value) (value)
#define kill_dependency(y) (y)

#define atomic_thread_fence(order) __atomic_thread_fence(order)
#define atomic_signal_fence(order) __atomic_signal_fence(order)
#define atomic_is_lock_free(obj)   __atomic_is_lock_free(sizeof(*(obj)), (obj))

typedef _Atomic(_Bool)              atomic_bool;
typedef _Atomic(char)               atomic_char;
typedef _Atomic(signed char)        atomic_schar;
typedef _Atomic(unsigned char)      atomic_uchar;
typedef _Atomic(short)              atomic_short;
typedef _Atomic(unsigned short)     atomic_ushort;
typedef _Atomic(int)                atomic_int;
typedef _Atomic(unsigned int)       atomic_uint;
typedef _Atomic(long)               atomic_long;
typedef _Atomic(unsigned long)      atomic_ulong;
typedef _Atomic(long long)          atomic_llong;
typedef _Atomic(unsigned long long) atomic_ullong;
typedef _Atomic(uint_least16_t)     atomic_char16_t;
typedef _Atomic(uint_least32_t)     atomic_char32_t;
typedef _Atomic(wchar_t)            atomic_wchar_t;
typedef _Atomic(int_least8_t)       atomic_int_least8_t;
typedef _Atomic(uint_least8_t)      atomic_uint_least8_t;
typedef _Atomic(int_least16_t)      atomic_int_least16_t;
typedef _Atomic(uint_least16_t)     atomic_uint_least16_t;
typedef _Atomic(int_least32_t)      atomic_int_least32_t;
typedef _Atomic(uint_least32_t)     atomic_uint_least32_t;
typedef _Atomic(int_least64_t)      atomic_int_least64_t;
typedef _Atomic(uint_least64_t)     atomic_uint_least64_t;
typedef _Atomic(int_fast8_t)        atomic_int_fast8_t;
typedef _Atomic(uint_fast8_t)       atomic_uint_fast8_t;
typedef _Atomic(int_fast16_t)       atomic_int_fast16_t;
typedef _Atomic(uint_fast16_t)      atomic_uint_fast16_t;
typedef _Atomic(int_fast32_t)       atomic_int_fast32_t;
typedef _Atomic(uint_fast32_t)      atomic_uint_fast32_t;
typedef _Atomic(int_fast64_t)       atomic_int_fast64_t;
typedef _Atomic(uint_fast64_t)      atomic_uint_fast64_t;
typedef _Atomic(intptr_t)           atomic_intptr_t;
typedef _Atomic(uintptr_t)          atomic_uintptr_t;
typedef _Atomic(size_t)             atomic_size_t;
typedef _Atomic(ptrdiff_t)          atomic_ptrdiff_t;
typedef _Atomic(intmax_t)           atomic_intmax_t;
typedef _Atomic(uintmax_t)          atomic_uintmax_t;

#if defined(__clang__)

#define atomic_init(obj, value) __c11_atomic_init((obj), (value))
#define atomic_store_explicit(obj, value, order) __c11_atomic_store((obj), (value), (order))
#define atomic_load_explicit(obj, order) __c11_atomic_load((obj), (order))
#define atomic_exchange_explicit(obj, value, order) __c11_atomic_exchange((obj), (value), (order))
#define atomic_compare_exchange_strong_explicit(obj, expected, desired, success, failure) \
	__c11_atomic_compare_exchange_strong((obj), (expected), (desired), (success), (failure))
#define atomic_compare_exchange_weak_explicit(obj, expected, desired, success, failure) \
	__c11_atomic_compare_exchange_weak((obj), (expected), (desired), (success), (failure))
#define atomic_fetch_add_explicit(obj, arg, order) __c11_atomic_fetch_add((obj), (arg), (order))
#define atomic_fetch_sub_explicit(obj, arg, order) __c11_atomic_fetch_sub((obj), (arg), (order))
#define atomic_fetch_or_explicit(obj, arg, order)  __c11_atomic_fetch_or((obj), (arg), (order))
#define atomic_fetch_xor_explicit(obj, arg, order) __c11_atomic_fetch_xor((obj), (arg), (order))
#define atomic_fetch_and_explicit(obj, arg, order) __c11_atomic_fetch_and((obj), (arg), (order))

#else

/* The value of an atomic object, as an ordinary value of the same type: the
 * comma drops the qualifier. */
#define __OKM_ATOMIC_VALUE(obj) __typeof__((void)0, *(obj))

#define atomic_init(obj, value) atomic_store_explicit((obj), (value), memory_order_relaxed)
#define atomic_store_explicit(obj, value, order) __extension__({ \
	__auto_type __okm_p = (obj); \
	__OKM_ATOMIC_VALUE(__okm_p) __okm_v = (value); \
	__atomic_store(__okm_p, &__okm_v, (order)); })
#define atomic_load_explicit(obj, order) __extension__({ \
	__auto_type __okm_p = (obj); \
	__OKM_ATOMIC_VALUE(__okm_p) __okm_r; \
	__atomic_load(__okm_p, &__okm_r, (order)); \
	__okm_r; })
#define atomic_exchange_explicit(obj, value, order) __extension__({ \
	__auto_type __okm_p = (obj); \
	__OKM_ATOMIC_VALUE(__okm_p) __okm_v = (value), __okm_r; \
	__atomic_exchange(__okm_p, &__okm_v, &__okm_r, (order)); \
	__okm_r; })
#define __OKM_ATOMIC_CAS(obj, expected, desired, weak, success, failure) __extension__({ \
	__auto_type __okm_p = (obj); \
	__OKM_ATOMIC_VALUE(__okm_p) __okm_d = (desired); \
	__atomic_compare_exchange(__okm_p, (expected), &__okm_d, (weak), (success), (failure)); })
#define atomic_compare_exchange_strong_explicit(obj, expected, desired, success, failure) \
	__OKM_ATOMIC_CAS((obj), (expected), (desired), 0, (success), (failure))
#define atomic_compare_exchange_weak_explicit(obj, expected, desired, success, failure) \
	__OKM_ATOMIC_CAS((obj), (expected), (desired), 1, (success), (failure))
#define atomic_fetch_add_explicit(obj, arg, order) __atomic_fetch_add((obj), (arg), (order))
#define atomic_fetch_sub_explicit(obj, arg, order) __atomic_fetch_sub((obj), (arg), (order))
#define atomic_fetch_or_explicit(obj, arg, order)  __atomic_fetch_or((obj), (arg), (order))
#define atomic_fetch_xor_explicit(obj, arg, order) __atomic_fetch_xor((obj), (arg), (order))
#define atomic_fetch_and_explicit(obj, arg, order) __atomic_fetch_and((obj), (arg), (order))

#endif

#define atomic_store(obj, value) atomic_store_explicit((obj), (value), memory_order_seq_cst)
#define atomic_load(obj) atomic_load_explicit((obj), memory_order_seq_cst)
#define atomic_exchange(obj, value) atomic_exchange_explicit((obj), (value), memory_order_seq_cst)
#define atomic_compare_exchange_strong(obj, expected, desired) \
	atomic_compare_exchange_strong_explicit((obj), (expected), (desired), \
	                                        memory_order_seq_cst, memory_order_seq_cst)
#define atomic_compare_exchange_weak(obj, expected, desired) \
	atomic_compare_exchange_weak_explicit((obj), (expected), (desired), \
	                                      memory_order_seq_cst, memory_order_seq_cst)
#define atomic_fetch_add(obj, arg) atomic_fetch_add_explicit((obj), (arg), memory_order_seq_cst)
#define atomic_fetch_sub(obj, arg) atomic_fetch_sub_explicit((obj), (arg), memory_order_seq_cst)
#define atomic_fetch_or(obj, arg)  atomic_fetch_or_explicit((obj), (arg), memory_order_seq_cst)
#define atomic_fetch_xor(obj, arg) atomic_fetch_xor_explicit((obj), (arg), memory_order_seq_cst)
#define atomic_fetch_and(obj, arg) atomic_fetch_and_explicit((obj), (arg), memory_order_seq_cst)

typedef struct atomic_flag { _Atomic(_Bool) _Value; } atomic_flag;
#define ATOMIC_FLAG_INIT { 0 }

#define atomic_flag_test_and_set_explicit(obj, order) \
	__atomic_test_and_set((void*)&(obj)->_Value, (order))
#define atomic_flag_clear_explicit(obj, order) \
	__atomic_clear((_Bool*)&(obj)->_Value, (order))
#define atomic_flag_test_and_set(obj) atomic_flag_test_and_set_explicit((obj), memory_order_seq_cst)
#define atomic_flag_clear(obj) atomic_flag_clear_explicit((obj), memory_order_seq_cst)

#endif /* !__cplusplus */

#endif /* _STDATOMIC_H */
