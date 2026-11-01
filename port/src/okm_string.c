/* The string and memory functions a program spends its time in, a vector at a
 * time (0.25.0: memcmp and strlen; 0.26.0: memcpy, memmove, memset, memchr,
 * strchr and strcmp, the wider vector where the processor has it, and loops of
 * four vectors).
 *
 * WHY. musl's own are written a byte or a word at a time, for every machine
 * alike; the system libraries beside this one choose an implementation for the
 * processor. Sampled in clangd.okb on Linux x86_64, strlen, memcmp and memcpy
 * took 18 % of the engine's time, most of it matching command-line options
 * against LLVM's option table, where the strings are short. bench/ times each
 * against the system's own (tools/bench-compare.sh).
 *
 * THE SHAPE. A length up to two vectors is handled where the function begins,
 * by loads that may overlap and no loop. Past that, four vectors an iteration,
 * the four compared together and the one that answered found afterwards. On
 * x86_64 every processor has 16-byte vectors (SSE2) and most have 32-byte ones
 * (AVX2): the processor is asked once (CPUID, and the system's consent to the
 * wider registers, XGETBV) and the functions of the wider kind are reached
 * through a table for the rest of the program --- the indirection the system
 * library's own selection makes as well. AArch64 has 16-byte vectors (NEON) on
 * every processor. Other architectures go a word at a time.
 *
 * NO LOAD CROSSES INTO A PAGE THE ARGUMENTS DO NOT REACH. A function that is
 * given a length reads within it: a tail is read by a load that ends at the
 * last byte, overlapping the one before. A function that looks for a terminator
 * reads vectors at addresses aligned to their size, and its loop of four reads
 * a block aligned to the block's size, which lies in one page; it may read past
 * the terminator within that block and never into the next page. strcmp, whose
 * two strings are aligned differently, reads vectors only while neither string
 * reaches the end of its page and a byte at a time across it.
 * tools/check-strings.sh compiles this file for the build machine and runs it
 * against guard pages, in every variant.
 *
 * Written with the compilers' vector extensions and their x86 builtins, which
 * GCC and Clang both provide, rather than with the processor vendors' headers,
 * which are the compiler's and not on this library's include path. */

/* A loop that stores one value, or copies, is a loop GCC may replace with a call
 * to memset or memcpy --- which here is a call to the function the loop is in.
 * `-ffreestanding' does not stop it; this does. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("no-tree-loop-distribute-patterns")
#endif

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define OKM_LE 1
#else
#define OKM_LE 0
#endif

#define OKM_PAGE_MIN 4096
#define INLINE static inline __attribute__((__always_inline__))
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

typedef unsigned char uc;
typedef uint64_t __attribute__((__may_alias__, __aligned__(1))) okm_u64u;
typedef uint32_t __attribute__((__may_alias__, __aligned__(1))) okm_u32u;
typedef uint16_t __attribute__((__may_alias__, __aligned__(1))) okm_u16u;
typedef size_t __attribute__((__may_alias__)) okm_word;

#define ONES ((size_t)-1 / 255)
#define HIGHS (ONES * 128)
#define HASZERO(x) (((x) - ONES) & ~(x) & HIGHS)

/* The bytes from `p' to the end of its page, by the smallest page there is. */
#define TO_PAGE_END(p) (OKM_PAGE_MIN - ((uintptr_t)(p) & (OKM_PAGE_MIN - 1)))

/* Up to sixteen bytes, by two overlapping loads of the widest size that fits,
 * all loads before any store: correct when the two ranges overlap, which is
 * what lets memmove use it too. */
INLINE void copy_small(uc* d, const uc* s, size_t n)
{
	if (n >= 8) {
		uint64_t a = *(const okm_u64u*)s, b = *(const okm_u64u*)(s + n - 8);
		*(okm_u64u*)d = a; *(okm_u64u*)(d + n - 8) = b;
	} else if (n >= 4) {
		uint32_t a = *(const okm_u32u*)s, b = *(const okm_u32u*)(s + n - 4);
		*(okm_u32u*)d = a; *(okm_u32u*)(d + n - 4) = b;
	} else if (n >= 2) {
		uint16_t a = *(const okm_u16u*)s, b = *(const okm_u16u*)(s + n - 2);
		*(okm_u16u*)d = a; *(okm_u16u*)(d + n - 2) = b;
	} else if (n) {
		*d = *s;
	}
}

/* Fewer than sixteen bytes compared in memcmp's order. */
INLINE int cmp_small(const uc* l, const uc* r, size_t n)
{
#if OKM_LE
	if (n >= 8) {
		uint64_t a = *(const okm_u64u*)l, b = *(const okm_u64u*)r;
		if (a != b) { const int i = __builtin_ctzll(a ^ b) >> 3; return l[i] - r[i]; }
		a = *(const okm_u64u*)(l + n - 8); b = *(const okm_u64u*)(r + n - 8);
		if (a != b) { const size_t i = n - 8 + (__builtin_ctzll(a ^ b) >> 3); return l[i] - r[i]; }
		return 0;
	}
	if (n >= 4) {
		uint32_t a = *(const okm_u32u*)l, b = *(const okm_u32u*)r;
		if (a != b) { const int i = __builtin_ctz(a ^ b) >> 3; return l[i] - r[i]; }
		a = *(const okm_u32u*)(l + n - 4); b = *(const okm_u32u*)(r + n - 4);
		if (a != b) { const size_t i = n - 4 + (__builtin_ctz(a ^ b) >> 3); return l[i] - r[i]; }
		return 0;
	}
#endif
	for (; n && *l == *r; n--, l++, r++);
	return n ? *l - *r : 0;
}

/* OKM_STRING_WORDS: the word-at-a-time functions on any processor, for
 * tools/check-strings.sh, which has no other way to run them. */
#if (defined(__x86_64__) || defined(__aarch64__)) && OKM_LE && !defined(OKM_STRING_WORDS)
#define OKM_VECTOR 1
#endif

#if OKM_VECTOR

/* --- sixteen bytes: SSE2 on x86_64, NEON on AArch64 ------------------------- */

typedef uc v16 __attribute__((__vector_size__(16), __aligned__(1), __may_alias__));
typedef uc v16a __attribute__((__vector_size__(16), __aligned__(16), __may_alias__));

INLINE v16 ld16(const void* p) { return *(const v16*)p; }
INLINE v16 lda16(const void* p) { return *(const v16a*)p; }
INLINE void st16(void* p, v16 v) { *(v16*)p = v; }
INLINE v16 splat16(uc c) { return (v16){ c, c, c, c, c, c, c, c, c, c, c, c, c, c, c, c }; }

/* A comparison's lanes as the byte vector the masks below take; the
 * comparison's own type is the compiler's signed vector. */
#define EQ16(a, b) ((v16)((a) == (b)))

/* A mask of a vector of lanes that are 0 or 0xff: one bit per byte on x86_64,
 * four on AArch64. AT16(m) is the first such byte; FULL16 the mask of all. */
#if defined(__x86_64__)
typedef unsigned m16_t;
INLINE m16_t mask16(v16 v) { return (unsigned)__builtin_ia32_pmovmskb128((__attribute__((__vector_size__(16))) char)v); }
#define AT16(m) ((size_t)__builtin_ctz(m))
#define BITS16 1
#define FULL16 0xffffu
#else
typedef uint64_t m16_t;
typedef unsigned short v8h __attribute__((__vector_size__(16)));
typedef uc v8b __attribute__((__vector_size__(8)));
/* Each byte's 0 or 0xff narrowed to four bits: the shift and narrowing the
 * compiler emits as one SHRN. */
INLINE m16_t mask16(v16 v)
{
	v8b n = __builtin_convertvector((v8h)v >> 4, v8b);
	uint64_t m;
	__builtin_memcpy(&m, &n, 8);
	return m;
}
#define AT16(m) ((size_t)(__builtin_ctzll(m) >> 2))
#define BITS16 4
#define FULL16 (~(uint64_t)0)
#endif

/* The smaller of each pair of lanes, unsigned. Four vectors are reduced to one
 * by it and one comparison tests all four: a lane is zero in the minimum where
 * it is zero in any. */
#if defined(__has_builtin) && __has_builtin(__builtin_elementwise_min)
#define MIN16(a, b) ((v16)__builtin_elementwise_min((a), (b)))
#elif defined(__x86_64__)
#define MIN16(a, b) ((v16)__builtin_ia32_pminub128((__attribute__((__vector_size__(16))) char)(a), \
                                                    (__attribute__((__vector_size__(16))) char)(b)))
#else
INLINE v16 min16_(v16 a, v16 b) { const v16 m = (v16)(a < b); return (a & m) | (b & ~m); }
#define MIN16(a, b) min16_((a), (b))
#endif

/* Forward, n > 32. Every load precedes the store that could reach it, which is
 * what lets memmove copy forward through it when the destination is below. */
INLINE void copy16(uc* d, const uc* s, size_t n)
{
	if (n <= 64) {
		const v16 a = ld16(s), b = ld16(s + 16), c = ld16(s + n - 32), e = ld16(s + n - 16);
		st16(d, a); st16(d + 16, b); st16(d + n - 32, c); st16(d + n - 16, e);
		return;
	}
	/* The first 16 bytes and the last 64 are loaded before anything is
	 * stored; between them the stores are aligned to 16. */
	const v16 h = ld16(s);
	const v16 t0 = ld16(s + n - 64), t1 = ld16(s + n - 48), t2 = ld16(s + n - 32), t3 = ld16(s + n - 16);
	uc* const end = d + n - 64;
	const size_t skip = 16 - ((uintptr_t)d & 15);
	uc* q = d + skip;
	const uc* p = s + skip;
	for (; q < end; q += 64, p += 64) {
		const v16 a = ld16(p), b = ld16(p + 16), c = ld16(p + 32), e = ld16(p + 48);
		*(v16a*)q = a; *(v16a*)(q + 16) = b; *(v16a*)(q + 32) = c; *(v16a*)(q + 48) = e;
	}
	st16(d, h);
	st16(end, t0); st16(end + 16, t1); st16(end + 32, t2); st16(end + 48, t3);
}

/* Backward, n > 64, the destination above the source and overlapping it. */
INLINE void copyback16(uc* d, const uc* s, size_t n)
{
	const v16 h0 = ld16(s), h1 = ld16(s + 16), h2 = ld16(s + 32), h3 = ld16(s + 48);
	while (n > 64) {
		n -= 64;
		const v16 a = ld16(s + n), b = ld16(s + n + 16), c = ld16(s + n + 32), e = ld16(s + n + 48);
		st16(d + n, a); st16(d + n + 16, b); st16(d + n + 32, c); st16(d + n + 48, e);
	}
	st16(d, h0); st16(d + 16, h1); st16(d + 32, h2); st16(d + 48, h3);
}

/* n > 32. */
INLINE void set16(uc* d, uc c, size_t n)
{
	const v16 v = splat16(c);
	if (n <= 64) { st16(d, v); st16(d + 16, v); st16(d + n - 32, v); st16(d + n - 16, v); return; }
	uc* const end = d + n - 64;
	st16(d, v);
	for (uc* q = (uc*)(((uintptr_t)d + 16) & ~(uintptr_t)15); q < end; q += 64) {
		*(v16a*)q = v; *(v16a*)(q + 16) = v; *(v16a*)(q + 32) = v; *(v16a*)(q + 48) = v;
	}
	st16(end, v); st16(end + 16, v); st16(end + 32, v); st16(end + 48, v);
}

/* Returns from the caller with the first differing byte of one vector's worth
 * at `at', if there is one. */
#define DIFF16(l, r, at) do { \
	const m16_t m_ = FULL16 & ~mask16(EQ16(ld16((l) + (at)), ld16((r) + (at)))); \
	if (m_) { const size_t i_ = (at) + AT16(m_); return (l)[i_] - (r)[i_]; } \
} while (0)

/* n >= 16. */
INLINE int cmp16(const uc* l, const uc* r, size_t n)
{
	if (n <= 32) { DIFF16(l, r, 0); DIFF16(l, r, n - 16); return 0; }
	size_t i = 0;
	for (; i + 64 <= n; i += 64) {
		const v16 e = EQ16(ld16(l + i), ld16(r + i)) & EQ16(ld16(l + i + 16), ld16(r + i + 16))
		            & EQ16(ld16(l + i + 32), ld16(r + i + 32)) & EQ16(ld16(l + i + 48), ld16(r + i + 48));
		if (UNLIKELY(mask16(e) != FULL16)) {
			DIFF16(l, r, i); DIFF16(l, r, i + 16); DIFF16(l, r, i + 32); DIFF16(l, r, i + 48);
		}
	}
	for (; i + 16 <= n; i += 16) DIFF16(l, r, i);
	if (i < n) DIFF16(l, r, n - 16);
	return 0;
}

/* The first byte at or after `s' and below the address `end' that is `c' (or,
 * with `nul', that is `c' or zero); null when there is none. Addresses are
 * compared as integers: `end' may lie beyond every object, where a pointer may
 * not be formed (measured: Clang 18, shown s + SIZE_MAX, answered a length of
 * 2^64 - 13 for a string of 16). */
/* A vector's lanes as zero where the scan stops: `c' (x ^ c is zero there), and
 * with `nul' also the terminator (the smaller of x ^ c and x). */
INLINE v16 t16(v16 x, v16 vc, const int nul)
{
	v16 t = x ^ vc;
	if (nul) t = MIN16(t, x);
	return t;
}
#define T16(x) t16((x), vc, nul)
#define STOP16(t) mask16(EQ16((t), z))
#define FOUND16(p, m) do { \
	const uintptr_t q_ = (uintptr_t)(p) + AT16(m); \
	return q_ < end ? (const uc*)q_ : 0; \
} while (0)

INLINE const uc* scan16(const uc* s, uc c, const int nul, uintptr_t end)
{
	const v16 vc = splat16(c), z = { 0 };
	const uintptr_t off = (uintptr_t)s & 15;
	const uc* p = s - off;
	v16 x = lda16(p);
	m16_t m = STOP16(T16(x)) >> (off * BITS16);
	if (m) FOUND16(s, m);
	for (p += 16; (uintptr_t)p & 63; p += 16) {
		if ((uintptr_t)p >= end) return 0;
		x = lda16(p);
		m = STOP16(T16(x));
		if (m) FOUND16(p, m);
	}
	for (; (uintptr_t)p < end; p += 64) {
		const v16 x0 = lda16(p), x1 = lda16(p + 16), x2 = lda16(p + 32), x3 = lda16(p + 48);
		const v16 t0 = T16(x0), t1 = T16(x1), t2 = T16(x2), t3 = T16(x3);
		if (STOP16(MIN16(MIN16(t0, t1), MIN16(t2, t3)))) {
			if ((m = STOP16(t0))) FOUND16(p, m);
			if ((m = STOP16(t1))) FOUND16(p + 16, m);
			if ((m = STOP16(t2))) FOUND16(p + 32, m);
			FOUND16(p + 48, STOP16(t3));
		}
	}
	return 0;
}

/* Returns from the caller where `l' and `r' differ or `l' ends, if that is
 * within the vector at `i'. */
#define STEP16(l, r, i) do { \
	const v16 a_ = ld16((l) + (i)); \
	const m16_t m_ = FULL16 & (~mask16(EQ16(a_, ld16((r) + (i)))) | mask16(EQ16(a_, z))); \
	if (m_) { const size_t k_ = (i) + AT16(m_); return (l)[k_] - (r)[k_]; } \
} while (0)

INLINE int strcmp16(const uc* l, const uc* r)
{
	const v16 z = { 0 };
	for (;;) {
		size_t room = TO_PAGE_END(l) < TO_PAGE_END(r) ? TO_PAGE_END(l) : TO_PAGE_END(r);
		for (; room >= 64; room -= 64, l += 64, r += 64) {
			/* A lane is zero in min(a, a == b) where a and b differ or a ends. */
			const v16 a0 = ld16(l), a1 = ld16(l + 16), a2 = ld16(l + 32), a3 = ld16(l + 48);
			const v16 t = MIN16(MIN16(MIN16(a0, EQ16(a0, ld16(r))), MIN16(a1, EQ16(a1, ld16(r + 16)))),
			                    MIN16(MIN16(a2, EQ16(a2, ld16(r + 32))), MIN16(a3, EQ16(a3, ld16(r + 48)))));
			if (UNLIKELY(mask16(EQ16(t, z)))) { STEP16(l, r, 0); STEP16(l, r, 16); STEP16(l, r, 32); STEP16(l, r, 48); }
		}
		for (; room >= 16; room -= 16, l += 16, r += 16) STEP16(l, r, 0);
		/* Across the nearer page's end, a byte at a time. */
		for (; room; room--, l++, r++)
			if (*l != *r || !*l) return *l - *r;
	}
}

#endif /* OKM_VECTOR */

/* --- thirty-two bytes: AVX2, chosen on x86_64 when the processor has it ------ */

#if defined(__x86_64__) && OKM_VECTOR
#define AVX2 __attribute__((__target__("avx2")))
#define AVX2_INLINE static inline __attribute__((__always_inline__, __target__("avx2")))

typedef uc v32 __attribute__((__vector_size__(32), __aligned__(1), __may_alias__));
typedef uc v32a __attribute__((__vector_size__(32), __aligned__(32), __may_alias__));

AVX2_INLINE v32 ld32(const void* p) { return *(const v32*)p; }
AVX2_INLINE v32 lda32(const void* p) { return *(const v32a*)p; }
AVX2_INLINE void st32(void* p, v32 v) { *(v32*)p = v; }
AVX2_INLINE unsigned mask32(v32 v)
{
	return (unsigned)__builtin_ia32_pmovmskb256((__attribute__((__vector_size__(32))) char)v);
}
#define EQ32(a, b) ((v32)((a) == (b)))

#if defined(__has_builtin) && __has_builtin(__builtin_elementwise_min)
#define MIN32(a, b) ((v32)__builtin_elementwise_min((a), (b)))
#else
#define MIN32(a, b) ((v32)__builtin_ia32_pminub256((__attribute__((__vector_size__(32))) char)(a), \
                                                    (__attribute__((__vector_size__(32))) char)(b)))
#endif

AVX2_INLINE void copy32(uc* d, const uc* s, size_t n)
{
	if (n <= 64) {
		const v32 a = ld32(s), b = ld32(s + n - 32);
		st32(d, a); st32(d + n - 32, b);
		return;
	}
	if (n <= 128) {
		const v32 a = ld32(s), b = ld32(s + 32), c = ld32(s + n - 64), e = ld32(s + n - 32);
		st32(d, a); st32(d + 32, b); st32(d + n - 64, c); st32(d + n - 32, e);
		return;
	}
	/* The first 32 bytes and the last 128 are loaded before anything is
	 * stored; between them the stores are aligned to 32, which a store that
	 * straddles two lines of the cache is not. */
	const v32 h = ld32(s);
	const v32 t0 = ld32(s + n - 128), t1 = ld32(s + n - 96), t2 = ld32(s + n - 64), t3 = ld32(s + n - 32);
	uc* const end = d + n - 128;
	const size_t skip = 32 - ((uintptr_t)d & 31);
	uc* q = d + skip;
	const uc* p = s + skip;
	for (; q < end; q += 128, p += 128) {
		const v32 a = ld32(p), b = ld32(p + 32), c = ld32(p + 64), e = ld32(p + 96);
		*(v32a*)q = a; *(v32a*)(q + 32) = b; *(v32a*)(q + 64) = c; *(v32a*)(q + 96) = e;
	}
	st32(d, h);
	st32(end, t0); st32(end + 32, t1); st32(end + 64, t2); st32(end + 96, t3);
}

AVX2_INLINE void copyback32(uc* d, const uc* s, size_t n)
{
	if (n <= 128) {
		const v32 a = ld32(s), b = ld32(s + 32), c = ld32(s + n - 64), e = ld32(s + n - 32);
		st32(d, a); st32(d + 32, b); st32(d + n - 64, c); st32(d + n - 32, e);
		return;
	}
	const v32 h0 = ld32(s), h1 = ld32(s + 32), h2 = ld32(s + 64), h3 = ld32(s + 96);
	while (n > 128) {
		n -= 128;
		const v32 a = ld32(s + n), b = ld32(s + n + 32), c = ld32(s + n + 64), e = ld32(s + n + 96);
		st32(d + n, a); st32(d + n + 32, b); st32(d + n + 64, c); st32(d + n + 96, e);
	}
	st32(d, h0); st32(d + 32, h1); st32(d + 64, h2); st32(d + 96, h3);
}

AVX2_INLINE void set32(uc* d, uc c, size_t n)
{
	v32 v;
	for (int i = 0; i < 32; i++) v[i] = c;
	if (n <= 64) { st32(d, v); st32(d + n - 32, v); return; }
	if (n <= 128) { st32(d, v); st32(d + 32, v); st32(d + n - 64, v); st32(d + n - 32, v); return; }
	/* The first 32 bytes and the last 128 unaligned, the stores between them
	 * aligned to 32. */
	uc* const end = d + n - 128;
	st32(d, v);
	for (uc* q = (uc*)(((uintptr_t)d + 32) & ~(uintptr_t)31); q < end; q += 128) {
		*(v32a*)q = v; *(v32a*)(q + 32) = v; *(v32a*)(q + 64) = v; *(v32a*)(q + 96) = v;
	}
	st32(end, v); st32(end + 32, v); st32(end + 64, v); st32(end + 96, v);
}

#define DIFF32(l, r, at) do { \
	const unsigned m_ = ~mask32(EQ32(ld32((l) + (at)), ld32((r) + (at)))); \
	if (m_) { const size_t i_ = (at) + (size_t)__builtin_ctz(m_); return (l)[i_] - (r)[i_]; } \
} while (0)

/* n > 32. */
AVX2_INLINE int cmp32(const uc* l, const uc* r, size_t n)
{
	if (n <= 64) { DIFF32(l, r, 0); DIFF32(l, r, n - 32); return 0; }
	if (n <= 128) { DIFF32(l, r, 0); DIFF32(l, r, 32); DIFF32(l, r, n - 64); DIFF32(l, r, n - 32); return 0; }
	size_t i = 0;
	for (; i + 128 <= n; i += 128) {
		const v32 e = EQ32(ld32(l + i), ld32(r + i)) & EQ32(ld32(l + i + 32), ld32(r + i + 32))
		            & EQ32(ld32(l + i + 64), ld32(r + i + 64)) & EQ32(ld32(l + i + 96), ld32(r + i + 96));
		if (UNLIKELY(mask32(e) != 0xffffffffu)) {
			DIFF32(l, r, i); DIFF32(l, r, i + 32); DIFF32(l, r, i + 64); DIFF32(l, r, i + 96);
		}
	}
	for (; i + 32 <= n; i += 32) DIFF32(l, r, i);
	if (i < n) DIFF32(l, r, n - 32);
	return 0;
}

AVX2_INLINE v32 t32(v32 x, v32 vc, const int nul)
{
	v32 t = x ^ vc;
	if (nul) t = MIN32(t, x);
	return t;
}
#define T32(x) t32((x), vc, nul)
#define STOP32(t) mask32(EQ32((t), z))
#define FOUND32(p, m) do { \
	const uintptr_t q_ = (uintptr_t)(p) + (size_t)__builtin_ctz(m); \
	return q_ < end ? (const uc*)q_ : 0; \
} while (0)

AVX2_INLINE const uc* scan32(const uc* s, uc c, const int nul, uintptr_t end)
{
	v32 vc, z = { 0 };
	for (int i = 0; i < 32; i++) vc[i] = c;
	const uintptr_t off = (uintptr_t)s & 31;
	const uc* p = s - off;
	v32 x = lda32(p);
	unsigned m = STOP32(T32(x)) >> off;
	if (m) FOUND32(s, m);
	for (p += 32; (uintptr_t)p & 127; p += 32) {
		if ((uintptr_t)p >= end) return 0;
		x = lda32(p);
		m = STOP32(T32(x));
		if (m) FOUND32(p, m);
	}
	for (; (uintptr_t)p < end; p += 128) {
		const v32 x0 = lda32(p), x1 = lda32(p + 32), x2 = lda32(p + 64), x3 = lda32(p + 96);
		const v32 t0 = T32(x0), t1 = T32(x1), t2 = T32(x2), t3 = T32(x3);
		if (STOP32(MIN32(MIN32(t0, t1), MIN32(t2, t3)))) {
			if ((m = STOP32(t0))) FOUND32(p, m);
			if ((m = STOP32(t1))) FOUND32(p + 32, m);
			if ((m = STOP32(t2))) FOUND32(p + 64, m);
			FOUND32(p + 96, STOP32(t3));
		}
	}
	return 0;
}

#define STEP32(l, r, i) do { \
	const v32 a_ = ld32((l) + (i)); \
	const unsigned m_ = ~mask32(EQ32(a_, ld32((r) + (i)))) | mask32(EQ32(a_, z)); \
	if (m_) { const size_t k_ = (i) + (size_t)__builtin_ctz(m_); return (l)[k_] - (r)[k_]; } \
} while (0)

AVX2_INLINE int strcmp32(const uc* l, const uc* r)
{
	const v32 z = { 0 };
	for (;;) {
		size_t room = TO_PAGE_END(l) < TO_PAGE_END(r) ? TO_PAGE_END(l) : TO_PAGE_END(r);
		for (; room >= 128; room -= 128, l += 128, r += 128) {
			const v32 a0 = ld32(l), a1 = ld32(l + 32), a2 = ld32(l + 64), a3 = ld32(l + 96);
			const v32 t = MIN32(MIN32(MIN32(a0, EQ32(a0, ld32(r))), MIN32(a1, EQ32(a1, ld32(r + 32)))),
			                    MIN32(MIN32(a2, EQ32(a2, ld32(r + 64))), MIN32(a3, EQ32(a3, ld32(r + 96)))));
			if (UNLIKELY(mask32(EQ32(t, z)))) { STEP32(l, r, 0); STEP32(l, r, 32); STEP32(l, r, 64); STEP32(l, r, 96); }
		}
		for (; room >= 32; room -= 32, l += 32, r += 32) STEP32(l, r, 0);
		for (; room; room--, l++, r++)
			if (*l != *r || !*l) return *l - *r;
	}
}

#endif /* AVX2 */

/* --- the eight functions, over sixteen bytes -------------------------------- */

#if OKM_VECTOR

INLINE void set_small(uc* d, uc b, size_t n)   /* n < 16 */
{
	const uint64_t w = (uint64_t)b * 0x0101010101010101ull;
	if (n >= 8) { *(okm_u64u*)d = w; *(okm_u64u*)(d + n - 8) = w; }
	else if (n >= 4) { *(okm_u32u*)d = (uint32_t)w; *(okm_u32u*)(d + n - 4) = (uint32_t)w; }
	else if (n) { d[0] = b; d[n / 2] = b; d[n - 1] = b; }
}

/* Every load before any store, 16 < n <= 64: memmove's short overlapping case. */
INLINE void move_short(uc* d, const uc* s, size_t n)
{
	v16 v[4];
	const size_t k = (n + 15) / 16;
	for (size_t i = 0; i + 1 < k; i++) v[i] = ld16(s + 16 * i);
	v[k - 1] = ld16(s + n - 16);
	for (size_t i = 0; i + 1 < k; i++) st16(d + 16 * i, v[i]);
	st16(d + n - 16, v[k - 1]);
}

/* Forward, without `restrict': memmove copies forward through it when the
 * destination begins below the source. */
INLINE void* memcpy16(uc* d, const uc* s, size_t n)
{
	if (n <= 16) copy_small(d, s, n);
	else if (n <= 32) { const v16 a = ld16(s), b = ld16(s + n - 16); st16(d, a); st16(d + n - 16, b); }
	else copy16(d, s, n);
	return d;
}

INLINE void* memmove16(uc* d, const uc* s, size_t n)
{
	/* Forward is right unless the destination begins inside the source. */
	if ((uintptr_t)d - (uintptr_t)s >= n) return memcpy16(d, s, n);
	if (n <= 16) copy_small(d, s, n);
	else if (n <= 64) move_short(d, s, n);
	else copyback16(d, s, n);
	return d;
}

INLINE void* memset16(uc* d, uc b, size_t n)
{
	if (n < 16) set_small(d, b, n);
	else if (n <= 32) { const v16 v = splat16(b); st16(d, v); st16(d + n - 16, v); }
	else set16(d, b, n);
	return d;
}

INLINE int memcmp16(const uc* l, const uc* r, size_t n)
{
	return n < 16 ? cmp_small(l, r, n) : cmp16(l, r, n);
}

/* THE FIRST FOUR VECTORS ARE READ WHERE THEY ARE, unaligned, when the page
 * holds them: most strings a program measures or compares end there, and the
 * aligned loop that follows would read some of them again. */

INLINE void* memchr16(const uc* s, uc c, size_t n)
{
	if (!n) return 0;
	const uintptr_t end = n > UINTPTR_MAX - (uintptr_t)s ? UINTPTR_MAX : (uintptr_t)s + n;
	const v16 vc = splat16(c);
	if (LIKELY(TO_PAGE_END(s) >= 64)) {
		for (size_t k = 0; k < 64; k += 16) {
			const m16_t m = mask16(EQ16(ld16(s + k), vc));
			if (m) { const uintptr_t q = (uintptr_t)s + k + AT16(m); return q < end ? (void*)q : 0; }
			if (n <= k + 16) return 0;
		}
		return (void*)scan16(s + 64, c, 0, end);
	}
	return (void*)scan16(s, c, 0, end);
}

INLINE char* strchr16(const uc* s, uc c)
{
	const uc* q = 0;
	if (LIKELY(TO_PAGE_END(s) >= 64)) {
		const v16 vc = splat16(c), z = { 0 };
		for (size_t k = 0; k < 64 && !q; k += 16) {
			const v16 x = ld16(s + k);
			const m16_t m = mask16(EQ16(x, vc) | EQ16(x, z));
			if (m) q = s + k + AT16(m);
		}
		if (!q) q = scan16(s + 64, c, 1, UINTPTR_MAX);
	}
	else q = scan16(s, c, 1, UINTPTR_MAX);
	return (q && *q == c) ? (char*)q : 0;
}

INLINE size_t strlen16(const uc* s)
{
	if (LIKELY(TO_PAGE_END(s) >= 64)) {
		const v16 z = { 0 };
		for (size_t k = 0; k < 64; k += 16) {
			const m16_t m = mask16(EQ16(ld16(s + k), z));
			if (m) return k + AT16(m);
		}
		return (size_t)(scan16(s + 64, 0, 0, UINTPTR_MAX) - s);
	}
	return (size_t)(scan16(s, 0, 0, UINTPTR_MAX) - s);
}

INLINE int strcmp16_first(const uc* l, const uc* r)
{
	if (LIKELY(TO_PAGE_END(l) >= 64 && TO_PAGE_END(r) >= 64)) {
		const v16 z = { 0 };
		STEP16(l, r, 0);
		STEP16(l, r, 16);
		STEP16(l, r, 32);
		STEP16(l, r, 48);
		l += 64; r += 64;
	}
	return strcmp16(l, r);
}

#endif /* OKM_VECTOR */

/* --- the eight functions, over thirty-two bytes ------------------------------ */

#if defined(__x86_64__) && OKM_VECTOR

AVX2_INLINE void* memcpy32(uc* d, const uc* s, size_t n)
{
	if (n <= 16) copy_small(d, s, n);
	else if (n <= 32) { const v16 a = ld16(s), b = ld16(s + n - 16); st16(d, a); st16(d + n - 16, b); }
	else copy32(d, s, n);
	return d;
}

AVX2_INLINE void* memmove32(uc* d, const uc* s, size_t n)
{
	if ((uintptr_t)d - (uintptr_t)s >= n) return memcpy32(d, s, n);
	if (n <= 16) copy_small(d, s, n);
	else if (n <= 64) move_short(d, s, n);
	else copyback32(d, s, n);
	return d;
}

AVX2_INLINE void* memset32(uc* d, uc b, size_t n)
{
	if (n < 16) set_small(d, b, n);
	else if (n <= 32) { const v16 v = splat16(b); st16(d, v); st16(d + n - 16, v); }
	else set32(d, b, n);
	return d;
}

AVX2_INLINE int memcmp32(const uc* l, const uc* r, size_t n)
{
	if (n < 16) return cmp_small(l, r, n);
	if (n <= 32) { DIFF16(l, r, 0); DIFF16(l, r, n - 16); return 0; }
	return cmp32(l, r, n);
}

AVX2_INLINE void* memchr32(const uc* s, uc c, size_t n)
{
	if (!n) return 0;
	const uintptr_t end = n > UINTPTR_MAX - (uintptr_t)s ? UINTPTR_MAX : (uintptr_t)s + n;
	if (LIKELY(TO_PAGE_END(s) >= 128)) {
		v32 vc;
		for (int i = 0; i < 32; i++) vc[i] = c;
		for (size_t k = 0; k < 128; k += 32) {
			const unsigned m = mask32(EQ32(ld32(s + k), vc));
			if (m) { const uintptr_t q = (uintptr_t)s + k + (size_t)__builtin_ctz(m); return q < end ? (void*)q : 0; }
			if (n <= k + 32) return 0;
		}
		return (void*)scan32(s + 128, c, 0, end);
	}
	return (void*)scan32(s, c, 0, end);
}

AVX2_INLINE char* strchr32(const uc* s, uc c)
{
	const uc* q = 0;
	if (LIKELY(TO_PAGE_END(s) >= 128)) {
		v32 vc, z = { 0 };
		for (int i = 0; i < 32; i++) vc[i] = c;
		for (size_t k = 0; k < 128 && !q; k += 32) {
			const v32 x = ld32(s + k);
			const unsigned m = mask32(EQ32(x, vc) | EQ32(x, z));
			if (m) q = s + k + __builtin_ctz(m);
		}
		if (!q) q = scan32(s + 128, c, 1, UINTPTR_MAX);
	}
	else q = scan32(s, c, 1, UINTPTR_MAX);
	return (q && *q == c) ? (char*)q : 0;
}

AVX2_INLINE size_t strlen32(const uc* s)
{
	if (LIKELY(TO_PAGE_END(s) >= 128)) {
		const v32 z = { 0 };
		for (size_t k = 0; k < 128; k += 32) {
			const unsigned m = mask32(EQ32(ld32(s + k), z));
			if (m) return k + (size_t)__builtin_ctz(m);
		}
		return (size_t)(scan32(s + 128, 0, 0, UINTPTR_MAX) - s);
	}
	return (size_t)(scan32(s, 0, 0, UINTPTR_MAX) - s);
}

AVX2_INLINE int strcmp32_first(const uc* l, const uc* r)
{
	if (LIKELY(TO_PAGE_END(l) >= 128 && TO_PAGE_END(r) >= 128)) {
		const v32 z = { 0 };
		STEP32(l, r, 0);
		STEP32(l, r, 32);
		STEP32(l, r, 64);
		STEP32(l, r, 96);
		l += 128; r += 128;
	}
	return strcmp32(l, r);
}

/* --- the choice, made once --------------------------------------------------- */

static int has_avx2(void)
{
	unsigned a, b, c, d;
	__asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0), "c"(0));
	if (a < 7) return 0;
	__asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
	const int osxsave = (c >> 27) & 1, avx = (c >> 28) & 1;
	if (!osxsave || !avx) return 0;
	unsigned lo, hi;
	__asm__ __volatile__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
	if ((lo & 6) != 6) return 0;   /* the system saves the wide registers */
	__asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
	return (b >> 5) & 1;
}

/* tools/check-strings.sh sets this to 0 to run the 16-byte functions on a
 * processor that has the wider ones. */
#ifndef OKM_STRING_AVX2_INITIAL
#define OKM_STRING_AVX2_INITIAL (-1)
#endif

/* Each function is reached through a word of its own, which first names a
 * function that makes the choice for all eight and then calls the choice:
 * a public function is one load and one jump, as the system library's own
 * selection through its link table is. */
#define OKM_STRING_FUNCTIONS(X) \
	X(memcpy,  void*,  (void* restrict d, const void* restrict s, size_t n), (d, s, n), \
	  memcpy16((uc*)d, (const uc*)s, n), memcpy32((uc*)d, (const uc*)s, n)) \
	X(memmove, void*,  (void* d, const void* s, size_t n), (d, s, n), \
	  memmove16((uc*)d, (const uc*)s, n), memmove32((uc*)d, (const uc*)s, n)) \
	X(memset,  void*,  (void* d, int c, size_t n), (d, c, n), \
	  memset16((uc*)d, (uc)c, n), memset32((uc*)d, (uc)c, n)) \
	X(memcmp,  int,    (const void* l, const void* r, size_t n), (l, r, n), \
	  memcmp16((const uc*)l, (const uc*)r, n), memcmp32((const uc*)l, (const uc*)r, n)) \
	X(memchr,  void*,  (const void* s, int c, size_t n), (s, c, n), \
	  memchr16((const uc*)s, (uc)c, n), memchr32((const uc*)s, (uc)c, n)) \
	X(strchr,  char*,  (const char* s, int c), (s, c), \
	  strchr16((const uc*)s, (uc)c), strchr32((const uc*)s, (uc)c)) \
	X(strlen,  size_t, (const char* s), (s), \
	  strlen16((const uc*)s), strlen32((const uc*)s)) \
	X(strcmp,  int,    (const char* l, const char* r), (l, r), \
	  strcmp16_first((const uc*)l, (const uc*)r), strcmp32_first((const uc*)l, (const uc*)r))

#define OKM_VARIANTS(name, ret, params, args, body16, body32) \
	static ret name##_sse2 params { return body16; } \
	AVX2 static ret name##_avx2 params { return body32; } \
	static ret name##_first params; \
	static ret (*g_##name) params = name##_first;
OKM_STRING_FUNCTIONS(OKM_VARIANTS)

static __attribute__((__noinline__)) void choose(void)
{
	const int wide = OKM_STRING_AVX2_INITIAL < 0 ? has_avx2() : OKM_STRING_AVX2_INITIAL;
#define OKM_CHOOSE(name, ret, params, args, body16, body32) \
	__atomic_store_n(&g_##name, wide ? name##_avx2 : name##_sse2, __ATOMIC_RELAXED);
	OKM_STRING_FUNCTIONS(OKM_CHOOSE)
}

#define OKM_PUBLIC(name, ret, params, args, body16, body32) \
	static ret name##_first params { choose(); return __atomic_load_n(&g_##name, __ATOMIC_RELAXED) args; } \
	ret name params { return __atomic_load_n(&g_##name, __ATOMIC_RELAXED) args; }
OKM_STRING_FUNCTIONS(OKM_PUBLIC)

#elif OKM_VECTOR

void* memcpy(void* restrict d, const void* restrict s, size_t n) { return memcpy16(d, s, n); }
void* memmove(void* d, const void* s, size_t n) { return memmove16(d, s, n); }
void* memset(void* d, int c, size_t n) { return memset16(d, (uc)c, n); }
int memcmp(const void* l, const void* r, size_t n) { return memcmp16(l, r, n); }
void* memchr(const void* s, int c, size_t n) { return memchr16(s, (uc)c, n); }
char* strchr(const char* s, int c) { return strchr16((const uc*)s, (uc)c); }
size_t strlen(const char* s) { return strlen16((const uc*)s); }
int strcmp(const char* l, const char* r) { return strcmp16_first((const uc*)l, (const uc*)r); }

#else /* a word at a time */

INLINE void copy_forward(uc* d, const uc* s, size_t n)
{
	if (n <= 16) { copy_small(d, s, n); return; }
	for (; n >= 8; n -= 8, d += 8, s += 8) *(okm_u64u*)d = *(const okm_u64u*)s;
	for (; n; n--) *d++ = *s++;
}

void* memcpy(void* restrict dest, const void* restrict src, size_t n)
{
	copy_forward(dest, src, n);
	return dest;
}

void* memmove(void* dest, const void* src, size_t n)
{
	uc* d = dest;
	const uc* s = src;
	if ((uintptr_t)d - (uintptr_t)s >= n) { copy_forward(d, s, n); return dest; }
	if (n <= 16) { copy_small(d, s, n); return dest; }
	for (; n >= 8; ) { n -= 8; *(okm_u64u*)(d + n) = *(const okm_u64u*)(s + n); }
	while (n) { n--; d[n] = s[n]; }
	return dest;
}

void* memset(void* dest, int c, size_t n)
{
	uc* d = dest;
	const uint64_t w = (uint64_t)(uc)c * 0x0101010101010101ull;
	for (; n >= 8; n -= 8, d += 8) *(okm_u64u*)d = w;
	for (; n; n--) *d++ = (uc)c;
	return dest;
}

int memcmp(const void* vl, const void* vr, size_t n)
{
	const uc *l = vl, *r = vr;
#if OKM_LE
	for (; n >= 16; n -= 8, l += 8, r += 8) {
		const uint64_t a = *(const okm_u64u*)l, b = *(const okm_u64u*)r;
		if (a != b) { const int i = __builtin_ctzll(a ^ b) >> 3; return l[i] - r[i]; }
	}
#endif
	return cmp_small(l, r, n);
}

void* memchr(const void* src, int c, size_t n)
{
	const uc* s = src;
	for (; n; n--, s++) if (*s == (uc)c) return (void*)s;
	return 0;
}

char* strchr(const char* s, int c)
{
	for (;; s++) {
		if (*(const uc*)s == (uc)c) return (char*)s;
		if (!*s) return 0;
	}
}

#if OKM_LE
size_t strlen(const char* s)
{
	uintptr_t off = (uintptr_t)s % sizeof(size_t);
	const okm_word* w = (const okm_word*)(s - off);
	/* The bytes before the string, made non-zero. */
	size_t x = *w | (off ? ((size_t)1 << (8 * off)) - 1 : 0);
	for (; !HASZERO(x); x = *++w);
	return (const char*)w - s + (__builtin_ctzll((unsigned long long)HASZERO(x)) >> 3);
}
#else
size_t strlen(const char* s)
{
	const char* a = s;
	for (; (uintptr_t)s % sizeof(size_t); s++) if (!*s) return s - a;
	const okm_word* w = (const void*)s;
	for (; !HASZERO(*w); w++);
	for (s = (const void*)w; *s; s++);
	return s - a;
}
#endif

int strcmp(const char* ls, const char* rs)
{
	const uc* l = (const uc*)ls;
	const uc* r = (const uc*)rs;
	for (; *l == *r && *l; l++, r++);
	return *l - *r;
}

#endif
