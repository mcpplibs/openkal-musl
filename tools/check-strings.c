/* port/src/okm_string.c, compiled for the build machine under other names and
 * checked against byte-at-a-time answers, with inaccessible pages on both sides
 * of every buffer. tools/check-strings.sh builds and runs it.
 *
 * WHAT A PROGRAM ABOVE THIS LIBRARY CANNOT CHECK. examples/strings runs the
 * functions where the library runs, and can make no page inaccessible: openkal
 * states no protection (region.h) and releases a reservation only whole. Here
 * the system's own C library and `mprotect' make the guard pages, so a load
 * past the bytes a function may read faults. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

void*  okm_memcpy(void*, const void*, size_t);
void*  okm_memmove(void*, const void*, size_t);
void*  okm_memset(void*, int, size_t);
int    okm_memcmp(const void*, const void*, size_t);
void*  okm_memchr(const void*, int, size_t);
char*  okm_strchr(const char*, int);
size_t okm_strlen(const char*);
int    okm_strcmp(const char*, const char*);

static long failures, cases;
static void fail(const char* what, size_t n, size_t a, size_t b)
{
	if (failures++ < 20) printf("FAIL %s n=%zu at %zu/%zu\n", what, n, a, b);
}
static int sign(int x) { return (x > 0) - (x < 0); }

/* A region of `len' bytes whose first and last pages are inaccessible; the
 * usable part is [lo, hi). */
static unsigned char *lo, *hi;
static void guarded(size_t len)
{
	const size_t page = (size_t)sysconf(_SC_PAGESIZE);
	unsigned char* p = mmap(0, len + 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) { perror("mmap"); exit(2); }
	if (mprotect(p, page, PROT_NONE) || mprotect(p + page + len, page, PROT_NONE)) { perror("mprotect"); exit(2); }
	lo = p + page; hi = lo + len;
}

static void fill(unsigned char* p, size_t n, unsigned seed)
{
	for (size_t i = 0; i < n; i++) p[i] = (unsigned char)(i * 131 + seed * 17 + 1) | 1;   /* no zero */
}

static size_t lengths[] = {
	0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 47, 48, 49, 63, 64, 65, 95, 96, 97,
	127, 128, 129, 255, 256, 257, 511, 1000, 1023, 1024, 1025, 4095, 4096, 4097, 10000,
};
#define NLEN (sizeof lengths / sizeof lengths[0])

int main(void)
{
	const size_t region = 4 * 4096;   /* a multiple of every page size */
	guarded(region);
	unsigned char* const buf2 = malloc(region);
	unsigned char* const ref = malloc(region);

	/* memcpy, memset: the destination and the source ending at the guard, and
	 * starting at it. */
	for (size_t k = 0; k < NLEN; k++) {
		const size_t n = lengths[k];
		for (size_t off = 0; off < 64; off += (n > 300 ? 7 : 1)) {
			unsigned char* src = hi - n - off;        /* ends `off' before the guard */
			unsigned char* dst = lo + off;            /* starts `off' after the guard */
			if (src < lo || dst + n > src) continue;   /* memcpy's ranges do not overlap */
			fill(src, n, (unsigned)k);
			memset(dst, 0xee, n);
			okm_memcpy(dst, src, n); cases++;
			if (memcmp(dst, src, n) != 0) fail("memcpy", n, off, 0);
			okm_memcpy(hi - n, lo, n); cases++;        /* the other way round */
			if (memcmp(hi - n, lo, n) != 0) fail("memcpy to the end", n, off, 0);
			okm_memset(src, 0x5a, n); cases++;
			for (size_t i = 0; i < n; i++) if (src[i] != 0x5a) { fail("memset", n, off, i); break; }
			okm_memset(lo + off, 0, n); cases++;
			for (size_t i = 0; i < n; i++) if (lo[off + i] != 0) { fail("memset 0", n, off, i); break; }
		}
	}

	/* memmove, every overlap in both directions. */
	for (size_t k = 0; k < NLEN && lengths[k] <= 1025; k++) {
		const size_t n = lengths[k];
		for (long shift = -70; shift <= 70; shift++) {
			unsigned char* base = lo + 128;
			if (base + n + 140 > hi) continue;
			fill(buf2, n + 200, (unsigned)shift);
			memcpy(base - 70, buf2, n + 140);
			memcpy(ref, base, n);
			okm_memmove(base + shift, base, n); cases++;
			if (memcmp(base + shift, ref, n) != 0) fail("memmove", n, (size_t)(shift + 70), 0);
		}
	}

	/* memcmp: equal, and one difference at each position in both directions;
	 * both ranges ending at the guard. */
	for (size_t k = 0; k < NLEN && lengths[k] <= 4096; k++) {
		const size_t n = lengths[k];
		unsigned char* a = hi - n;
		unsigned char* b = lo;
		fill(a, n, 3); memcpy(b, a, n);
		cases++;
		if (okm_memcmp(a, b, n) != 0) fail("memcmp equal", n, 0, 0);
		for (size_t i = 0; i < n; i += (n > 300 ? 13 : 1)) {
			const unsigned char keep = b[i];
			b[i] = (unsigned char)(keep + 1); cases++;
			if (sign(okm_memcmp(a, b, n)) != sign(memcmp(a, b, n))) fail("memcmp +", n, i, 0);
			b[i] = (unsigned char)(keep - 1); cases++;
			if (sign(okm_memcmp(a, b, n)) != sign(memcmp(a, b, n))) fail("memcmp -", n, i, 0);
			b[i] = keep;
		}
	}

	/* memchr: the byte at each position, and absent; the range ending at the
	 * guard and starting at it. */
	for (size_t k = 0; k < NLEN && lengths[k] <= 4096; k++) {
		const size_t n = lengths[k];
		for (int end = 0; end < 2; end++) {
			unsigned char* p = end ? hi - n : lo;
			fill(p, n, 5);
			for (size_t i = 0; i < n; i++) if (p[i] == 0x42) p[i] = 0x43;
			cases++;
			if (okm_memchr(p, 0x42, n) != 0) fail("memchr absent", n, end, 0);
			for (size_t i = 0; i < n; i += (n > 300 ? 11 : 1)) {
				p[i] = 0x42; cases++;
				if (okm_memchr(p, 0x42, n) != p + i) fail("memchr", n, end, i);
				if (i && okm_memchr(p, 0x42, i) != 0) fail("memchr short", n, end, i);
				p[i] = 0x43;
			}
		}
	}

	/* strlen, strchr: the terminator as the last byte before the guard, at
	 * every alignment. */
	for (size_t n = 0; n <= 4100; n += (n < 300 ? 1 : 37)) {
		char* s = (char*)hi - n - 1;
		if ((unsigned char*)s < lo) break;
		fill((unsigned char*)s, n, 7);
		s[n] = 0;
		cases++;
		if (okm_strlen(s) != n) fail("strlen", n, 0, 0);
		if (okm_strchr(s, 0) != s + n) fail("strchr nul", n, 0, 0);
		const int c = n ? (unsigned char)s[n / 2] : 'q';
		char* want = n ? strchr(s, c) : 0;
		cases++;
		if (okm_strchr(s, c) != want) fail("strchr", n, 0, 0);
		if (okm_strchr(s, 0x100 + 0x02) != strchr(s, 0x02)) fail("strchr high bits", n, 0, 0);
	}

	/* strcmp: two strings ending at the guard and starting at differing
	 * alignments, equal, and differing at each position. */
	for (size_t n = 0; n <= 6000; n += (n < 600 ? 1 : 97)) {
		for (size_t off = 0; off < 32; off += (n < 600 ? 3 : 5)) {
			char* a = (char*)hi - n - 1;
			char* b = (char*)lo + off;
			fill((unsigned char*)a, n, 9);
			a[n] = 0;
			memcpy(b, a, n + 1);
			cases++;
			if (okm_strcmp(a, b) != 0 || okm_strcmp(b, a) != 0) fail("strcmp equal", n, off, 0);
			for (size_t i = 0; i < n; i += (n > 100 ? 17 : 1)) {
				const char keep = b[i];
				b[i] = (char)(keep + 1); cases++;
				if (sign(okm_strcmp(a, b)) != sign(strcmp(a, b))) fail("strcmp +", n, off, i);
				b[i] = 0; cases++;
				if (sign(okm_strcmp(a, b)) != sign(strcmp(a, b))) fail("strcmp short", n, off, i);
				b[i] = keep;
			}
		}
	}

	printf("%ld cases, %ld failures\n", cases, failures);
	return failures != 0;
}
