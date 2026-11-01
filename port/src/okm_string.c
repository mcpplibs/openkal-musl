/* memcmp AND strlen A WORD OR A VECTOR AT A TIME (0.25.0).
 *
 * musl's own memcmp compares a byte at a time, and its strlen walks a byte at a
 * time to the first aligned word; the system libraries beside this one compare
 * in vectors. Sampled in clangd.okb on Linux x86_64, the two took 14 % of the
 * engine's time, most of it matching command-line options against LLVM's
 * option table; the native clangd spends a small fraction of that there.
 *
 * Every load is of a whole word or vector at an address aligned to its size, or
 * of bytes the caller passed, so a load never crosses into a page the string
 * does not reach: it may read bytes beyond the terminator within the aligned
 * unit, as musl's own strlen does, and none beyond. musl/src/string/memcmp.c
 * and strlen.c are excluded in the manifest, which names this file. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define OKM_LE 1
#else
#define OKM_LE 0
#endif

typedef uint64_t __attribute__((__may_alias__, __aligned__(1))) okm_u64u;
typedef size_t __attribute__((__may_alias__)) okm_word;

#define ONES ((size_t)-1 / 255)
#define HIGHS (ONES * 128)
#define HASZERO(x) (((x) - ONES) & ~(x) & HIGHS)

int memcmp(const void *vl, const void *vr, size_t n)
{
	const unsigned char *l = vl, *r = vr;
#if OKM_LE
	for (; n >= 8; n -= 8, l += 8, r += 8) {
		uint64_t a = *(const okm_u64u *)l, b = *(const okm_u64u *)r;
		if (a != b) {
			int i = __builtin_ctzll(a ^ b) >> 3;
			return l[i] - r[i];
		}
	}
#endif
	for (; n && *l == *r; n--, l++, r++);
	return n ? *l - *r : 0;
}

#if defined(__SSE2__)
typedef char okm_v16 __attribute__((__vector_size__(16), __aligned__(16)));

size_t strlen(const char *s)
{
	const okm_v16 zero = { 0 };
	uintptr_t off = (uintptr_t)s & 15;
	const okm_v16 *v = (const okm_v16 *)(s - off);
	unsigned mask = (unsigned)__builtin_ia32_pmovmskb128(*v == zero) >> off;
	if (mask) return __builtin_ctz(mask);
	for (;;) {
		mask = (unsigned)__builtin_ia32_pmovmskb128(*++v == zero);
		if (mask) return (const char *)v - s + __builtin_ctz(mask);
	}
}
#elif OKM_LE
size_t strlen(const char *s)
{
	uintptr_t off = (uintptr_t)s % sizeof(size_t);
	const okm_word *w = (const okm_word *)(s - off);
	/* The bytes before the string, made non-zero. */
	size_t x = *w | (off ? ((size_t)1 << (8 * off)) - 1 : 0);
	for (; !HASZERO(x); x = *++w);
	return (const char *)w - s + (__builtin_ctzll((unsigned long long)HASZERO(x)) >> 3);
}
#else
size_t strlen(const char *s)
{
	const char *a = s;
	for (; (uintptr_t)s % sizeof(size_t); s++) if (!*s) return s - a;
	const okm_word *w = (const void *)s;
	for (; !HASZERO(*w); w++);
	for (s = (const void *)w; *s; s++);
	return s - a;
}
#endif
