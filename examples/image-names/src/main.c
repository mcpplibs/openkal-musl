/* A name beneath a volume, spelled as its environment allows (port/src/okm_fd.c,
 * 0.27.0).
 *
 * On a system whose absolute names begin with a volume (`C:'), an editor names
 * a file with the volume's letter in the other case and either separator ---
 * `c:\Users\...' for the directory `C:/Users/...' --- and the environment takes
 * both as the same name. The C library compared the name with the directories
 * it was given byte for byte, so such a name reached none of them. Here the
 * directory the image runs in is named again, its letter's case changed and its
 * separators reversed, and a file written under one spelling is read under the
 * other. Elsewhere there is no volume, and nothing is asked. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

static int failures;

static int read_bytes(int fd, char* out)
{
	return read(fd, out, 5) == 5;
}
static void check(int ok, const char* what)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) failures++;
}

int main(void)
{
	char cwd[4096];
	check(getcwd(cwd, sizeof cwd) != 0, "the directory the image runs in is named");
	if (!(cwd[0] && cwd[1] == ':')) {
		printf("ok   no volume in %s: nothing to ask\n-- failures: %d --\n", cwd, failures);
		return failures != 0;
	}
	char written[4200], other[4200];
	snprintf(written, sizeof written, "%s/okm-names.tmp", cwd);
	strcpy(other, written);
	other[0] = (char)(other[0] >= 'a' ? other[0] - 'a' + 'A' : other[0] - 'A' + 'a');
	for (char* p = other; *p; ++p) if (*p == '/') *p = '\\';
	const int w = open(written, O_CREAT | O_WRONLY | O_TRUNC, 0600);
	check(w >= 0 && write(w, "names", 5) == 5, "a file is written under the name the image was given");
	if (w >= 0) close(w);
	char bytes[8] = { 0 };
	const int r = open(other, O_RDONLY);
	check(r >= 0 && read_bytes(r, bytes) && !memcmp(bytes, "names", 5),
	      "and read under its volume's other case and the other separator");
	if (r >= 0) close(r);
	printf("   %s\n   %s\n", written, other);
	unlink(written);
	printf("-- failures: %d --\n", failures);
	return failures != 0;
}
