/* What `[c-abi]` in the package manifest claims, checked rather than trusted.
 *
 * Before 0.15.0 this package presented Windows as LLP64 with a 16-bit
 * `wchar_t` --- the platform's own C runtime's data model, and not the one
 * musl or this port's other two targets use. `[c-abi]` now states the same
 * environment everywhere: POSIX presence, LP64, a 32-bit `wchar_t`. This
 * probe is the five observations that statement makes, asked of the compiler
 * and the library actually in use rather than assumed from the target name.
 * musl/PATCHES.md records the four patches removing that statement retired.
 */
#include <stdio.h>
#include <wchar.h>

static int failures = 0;
static void check(int ok, const char *what)
{
	if (!ok) { printf("FAIL: %s\n", what); failures++; }
	else printf("ok: %s\n", what);
}

int main(void)
{
	setbuf(stdout, NULL);
	printf("-- openkal-musl c-abi probe --\n");

	/* data-model = "arch-default": LP64 on every 64-bit target this port
	 * builds for, Windows included --- a `long` holds a pointer. */
	check(sizeof(long) == 8, "sizeof(long) == 8");

	/* wchar = 32: musl's own wchar_t, unconditionally. */
	check(sizeof(wchar_t) == 4, "sizeof(wchar_t) == 4");

	/* A wide-character literal compiles and carries a code point above
	 * U+FFFF. Before 0.15.0 this line did not compile on Windows at all ---
	 * deliberately, so that a narrower wchar_t could not silently truncate
	 * it; the three musl sources this build now compiles unpatched
	 * (src/stdio/vfwscanf.c, src/stdlib/wcstol.c, src/stdlib/wcstod.c) wrote
	 * their own wide literals around exactly this. */
	static const wchar_t grin[] = L"\U0001F600";
	check(sizeof(grin) / sizeof(grin[0]) == 2,
	      "a wide literal holds one code point above U+FFFF, plus the terminator");
	check((unsigned)grin[0] == 0x1F600u,
	      "the code point round-trips through the wide literal without truncation");

	/* presents = "posix": the environment identity, not the machine code.
	 * PE and Win64 are unchanged on Windows; what changes is only what the
	 * source text sees. */
#if defined(_WIN32) || defined(_WIN64) || defined(__MINGW32__) || defined(__MINGW64__)
	check(0, "_WIN32 (or _WIN64 / __MINGW32__) is not defined");
#else
	check(1, "_WIN32 is not defined");
#endif

#if defined(__unix__)
	check(1, "__unix__ is defined");
#else
	check(0, "__unix__ is defined");
#endif

	printf("-- failures: %d --\n", failures);
	return failures ? 1 : 0;
}
