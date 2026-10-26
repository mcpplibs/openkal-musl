/* A constructor that formats, allocates and reads the environment before
 * `main': the loader runs it before the image's entry, so the C library has to
 * be up by then (its own initialiser runs first). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char* g_early;

__attribute__((constructor)) static void early(void)
{
	g_early = malloc(64);
	snprintf(g_early, 64, "early %d %s", 42, getenv("PATH") ? "path" : "nopath");
}

int main(int argc, char** argv)
{
	(void)argv;
	const int ok = g_early && strncmp(g_early, "early 42 ", 9) == 0 && argc >= 1;
	printf("%s\n", g_early ? g_early : "(no early value)");
	printf("early-constructor: %s\n", ok ? "the C library was up before the constructor" : "FAIL");
	return ok ? 0 : 1;
}
