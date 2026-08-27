/* Names this library must NOT take from a program above it.
 *
 * Reported as mcpplibs/openkal-musl#13: a program built on this package could
 * not declare `hidden' as an ordinary identifier, because musl's INTERNAL
 * header overlay defines it as an attribute and this package published the path
 * it is built from.
 *
 * ⭐ THIS IS A COMPILE-TIME CRITERION AND IT IS THE WHOLE OF THE TEST. If any
 * of the three below is still a macro, this file does not compile: in C they
 * were emptied, so `static int hidden = 7;' becomes `static int  = 7;'. There
 * is nothing to run and nothing to compare --- the program exists so that
 * something LINKS and RUNS afterwards, which is what distinguishes a source
 * that compiles from a package that works.
 *
 * ⚠️ `restrict' IS NOT AMONG THEM, and its absence here is deliberate rather
 * than an oversight. musl's PUBLIC headers write it --- <stdio.h> declares
 * `fprintf(FILE *restrict, const char *restrict, ...)' --- so a C++ program
 * above this library needs the spelling defined and cannot have the name. That
 * is a property of musl's headers and would be one above a kernel too.
 * port/include/features.h records the division.
 */
#include <stdio.h>

static int hidden = 7;
static int weak = 11;
static int weak_alias = 23;

/* And as a function parameter and a member, which is where a macro that
 * survived would show differently. */
struct holder { int hidden; int weak; };

static int sum(int hidden, int weak, int weak_alias)
{
	struct holder h = { hidden, weak };
	return h.hidden + h.weak + weak_alias;
}

int main(void)
{
	setbuf(stdout, NULL);
	const int total = sum(hidden, weak, weak_alias);
	printf("hidden+weak+weak_alias = %d\n", total);
	/* 41, and the value is printed rather than merely computed so that a
	 * reader of the log sees the three names carried a value of the program's
	 * own choosing rather than an attribute. */
	const int failures = (total == 41) ? 0 : 1;
	if (failures) printf("FAIL: the three names did not carry their values\n");
	/* The same last line every probe in this repository prints, so that one
	 * runner reads all four. tools/run-probe.sh asserts both directions of it. */
	printf("-- failures: %d --\n", failures);
	return failures ? 1 : 0;
}
