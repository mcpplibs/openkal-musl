/* memcmp and strlen a word or a vector at a time (port/src/okm_string.c, 0.25.0).
 *
 * EVERY ALIGNMENT AND EVERY LENGTH, against the byte-at-a-time answer: both
 * arguments at each of sixteen offsets, lengths across several words and
 * vectors, and a difference at each position in both directions, since the
 * word comparison must still name the first differing byte and its sign. And
 * the property that makes reading ahead admissible: a string whose terminator
 * is the last byte before an unmapped page is measured without touching it. */
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int failures;
static void check(int ok, const char* what)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) failures++;
}

static int sign(int x) { return (x > 0) - (x < 0); }

static int reference_cmp(const unsigned char* l, const unsigned char* r, size_t n)
{
	for (size_t i = 0; i < n; i++) if (l[i] != r[i]) return l[i] - r[i];
	return 0;
}

int main(void)
{
	static unsigned char a[512], b[512];
	long wrong = 0, cases = 0;
	for (size_t la = 0; la < 16; la++)
		for (size_t lb = 0; lb < 16; lb++)
			for (size_t n = 0; n <= 80; n++) {
				for (size_t i = 0; i < n; i++) a[la + i] = b[lb + i] = (unsigned char)(i * 37 + 11);
				cases++;
				if (memcmp(a + la, b + lb, n) != 0) wrong++;
				for (size_t k = 0; k < n; k += (n > 24 ? 7 : 1)) {
					unsigned char keep = b[lb + k];
					b[lb + k] = (unsigned char)(keep + 1);
					cases++;
					if (sign(memcmp(a + la, b + lb, n)) != sign(reference_cmp(a + la, b + lb, n))) wrong++;
					b[lb + k] = (unsigned char)(keep - 1);
					cases++;
					if (sign(memcmp(a + la, b + lb, n)) != sign(reference_cmp(a + la, b + lb, n))) wrong++;
					b[lb + k] = keep;
				}
			}
	printf("memcmp: %ld cases\n", cases);
	check(wrong == 0, "memcmp: equal ranges answer 0 and the first difference decides the sign, at every alignment");

	unsigned char hi[16] = { 0x80 }, lo[16] = { 0x7f };
	check(memcmp(hi, lo, 16) > 0 && memcmp(lo, hi, 16) < 0, "memcmp: bytes compare as unsigned char");

	static char s[512];
	wrong = 0;
	for (size_t off = 0; off < 32; off++)
		for (size_t n = 0; n <= 200; n++) {
			memset(s, 'x', sizeof s);
			s[off + n] = 0;
			if (strlen(s + off) != n) wrong++;
		}
	check(wrong == 0, "strlen: every length from 0 to 200 at each of 32 offsets");

	/* High bytes are not terminators; only zero is. */
	memset(s, 0x80, 64);
	s[40] = 0;
	check(strlen(s) == 40, "strlen: bytes of value 0x80 and above are counted");
	memset(s, 0x01, 64);
	s[17] = 0;
	check(strlen(s) == 17, "strlen: bytes of value 1 are counted");

	long page = sysconf(_SC_PAGESIZE);
	char* two = mmap(0, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	int edge = two != MAP_FAILED && munmap(two + page, page) == 0;
	if (edge) {
		wrong = 0;
		for (size_t n = 0; n < 100; n++) {
			char* t = two + page - 1 - n;
			memset(t, 'y', n);
			t[n] = 0;
			if (strlen(t) != n) wrong++;
			if (n && memcmp(t, t, n + 1) != 0) wrong++;
		}
		check(wrong == 0, "a string ending at the last byte before an unmapped page is measured and compared without a fault");
	} else {
		check(0, "two pages were mapped and the second released");
	}
	printf("-- failures: %d --\n", failures);
	return failures != 0;
}
