#!/usr/bin/env python3
"""Run a program upon a pseudo-terminal and type at it.

    pty-keys.py <marker> <hex-bytes> <command> [arguments...]

The program is started with a pseudo-terminal for its three standard streams;
once <marker> appears in its output the bytes named by <hex-bytes> are written
into the terminal, and everything the program printed is written to this
script's standard output. The exit status is the program's, except that a
program which has not ended within the timeout is reported as 124 --- the
status `timeout' uses --- because a program that never returns is as much a
failure as one that returns wrongly.

WHY A PSEUDO-TERMINAL AND NOT `script -qec'. The keystrokes have to arrive
AFTER the program has established its mode. A keystroke that arrives before is
assembled into a line by the terminal, and the interrupt one ends the program:
the transcript then shows the defect this probe exists to detect on a port that
does not have it. `script' offers nowhere to wait, so the wait is here.

WHY THE OUTPUT IS COMPARED AND NOT ASSERTED HERE. The claim this probe carries
is a relation --- that a program above this port behaves as the same program
above the system's own C library --- so the script reports and the caller
compares. A script that asserted "0x03 arrives" would pass on a system whose
terminal does not deliver it at all.
"""
import os
import pty
import select
import sys
import time

TIMEOUT = 30.0


def main(argv):
    if len(argv) < 4:
        sys.stderr.write(__doc__)
        return 2
    marker, keys, command = argv[1].encode(), bytes.fromhex(argv[2]), argv[3:]

    pid, fd = pty.fork()
    if pid == 0:
        os.execvp(command[0], command)
        os._exit(127)

    out = bytearray()
    typed = False
    deadline = time.monotonic() + TIMEOUT
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            os.kill(pid, 9)
            os.waitpid(pid, 0)
            sys.stdout.buffer.write(bytes(out))
            sys.stdout.flush()
            sys.stderr.write("pty-keys: the program did not end within %gs\n" % TIMEOUT)
            return 124
        ready, _, _ = select.select([fd], [], [], min(remaining, 0.5))
        if ready:
            try:
                chunk = os.read(fd, 4096)
            except OSError:           # the far end closed: the program has gone
                chunk = b""
            if not chunk:
                break
            out += chunk
        if not typed and marker in out:
            # A pause before typing, because the marker is printed before the
            # read that receives the keystrokes begins. Bytes written into the
            # terminal are buffered by it either way; the pause keeps the
            # transcript in the order a reader expects.
            time.sleep(0.2)
            os.write(fd, keys)
            typed = True

    _, status = os.waitpid(pid, 0)
    sys.stdout.buffer.write(bytes(out))
    sys.stdout.flush()
    if os.WIFSIGNALED(status):
        sys.stderr.write("pty-keys: the program was ended by signal %d\n"
                         % os.WTERMSIG(status))
        return 128 + os.WTERMSIG(status)
    return os.WEXITSTATUS(status)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
