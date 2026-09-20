/* Does a terminal put into raw mode behave as one?
 *
 * The program this port was reported against (mcpplibs/openkal-musl#36),
 * reduced to what can be asserted by a harness rather than read by a person.
 * It is built twice --- above this package and above the system's own C
 * library --- and the two transcripts are compared, because the question is
 * not "does this port do something" but "does it do what a C library does".
 *
 * THE STRUCTURE IS POISONED BEFORE EVERY ENQUIRY. `tcgetattr' that reports
 * success and writes nothing is indistinguishable from one that worked, unless
 * the caller can tell what was there before; the sentinel is what makes the
 * difference visible. That was the quieter half of the report: the loud half
 * was `tcsetattr' refused with ENOTTY, and this program would have passed a
 * test that only checked the loud one.
 *
 * WHAT THE HARNESS TYPES, AND WHY THE MARKER EXISTS. The keystrokes are sent
 * after `reading' appears, because a keystroke that arrives before the mode is
 * established is assembled into a line by the terminal and the interrupt one
 * ends the program --- which is the defect, not the test. */
/* `cfmakeraw' is not ISO C, and this package presents the POSIX view of musl
 * rather than the BSD one (README, "The C environment this package presents").
 * The probe asks for the wider view explicitly, because the call under
 * examination is exactly the one a program reaches for. */
#define _GNU_SOURCE 1
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static void show(const char* tag, const struct termios* t) {
    printf("%s lflag_icanon=%d lflag_echo=%d lflag_isig=%d iflag_ixon=%d vmin=%d vtime=%d\r\n",
           tag,
           (t->c_lflag & ICANON) != 0,
           (t->c_lflag & ECHO) != 0,
           (t->c_lflag & ISIG) != 0,
           (t->c_iflag & IXON) != 0,
           (int)t->c_cc[VMIN], (int)t->c_cc[VTIME]);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("isatty %d\r\n", isatty(0));

    struct termios original;
    memset(&original, 0x5a, sizeof original);
    errno = 0;
    const int got = tcgetattr(0, &original);
    printf("tcgetattr rc=%d errno=%d\r\n", got, got == 0 ? 0 : errno);
    if (got != 0) { printf("no terminal\r\n"); return 1; }
    show("before", &original);

    struct termios raw = original;
    cfmakeraw(&raw);
    errno = 0;
    const int set = tcsetattr(0, TCSANOW, &raw);
    printf("tcsetattr rc=%d errno=%d\r\n", set, set == 0 ? 0 : errno);

    struct termios back;
    memset(&back, 0x5a, sizeof back);
    errno = 0;
    const int again = tcgetattr(0, &back);
    printf("tcgetattr(readback) rc=%d errno=%d\r\n", again, again == 0 ? 0 : errno);
    show("readback", &back);

    printf("reading\r\n");
    for (char c; read(0, &c, 1) == 1; ) {
        printf("byte 0x%02x\r\n", (unsigned char)c);
        if (c == 'q') break;
    }

    tcsetattr(0, TCSANOW, &original);
    struct termios restored;
    memset(&restored, 0x5a, sizeof restored);
    if (tcgetattr(0, &restored) == 0) show("restored", &restored);
    printf("done\r\n");
    return 0;
}
