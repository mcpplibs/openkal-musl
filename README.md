# openkal-musl

musl 1.2.5 redirected onto [openkal](https://github.com/mcpplibs/openkal).

The specification claims that porting one C library causes the software above it
to run on every implementation. This package is that port, and it exists so that
the claim can be checked rather than repeated.

```toml
[dependencies]
openkal-musl = "0.1.0"
```

That is the whole of a program's manifest. It names no implementation and no
platform: a C library is the one consumer that must know which implementation
it needs, so it declares that itself.

## What was changed, and what was not

musl reaches its kernel through seven inline functions declared once per
architecture. Replacing that one header is the whole of the redirection; the
1345 sources that make up the library are compiled unmodified.

`musl/PATCHES.md` lists the whole of what is not unmodified: **four patched
lines**, all of one kind — a machine word carried through a variable declared
`long`, which is not a machine word on one of the three targets — and **nine
replaced sources**. Five of the nine are replaced for the same reason: each
reads the shape of one particular environment rather than asking a kernel for
something.

| Source | What it reads directly | What replaces it |
| --- | --- | --- |
| `src/env/__libc_start_main.c` | the auxiliary vector Linux leaves on the initial stack | the arguments and named values, from `openkal.env` |
| `src/env/__init_tls.c` | the program's own ELF headers | nothing: openkal reports that a started context already observes thread-local storage |
| `src/thread/__set_thread_area.c` | the instruction that installs a thread pointer | an assignment to one variable |
| `src/thread/clone.c` | the system call that creates a thread | `kal_task_start` |
| `src/process/posix_spawn.c` | duplication of the calling image, then replacement | `kal_process_spawn`, which is the composite |

The other four were found by running rather than by reading: two carry a machine
word through a `long`, one refuses a working directory that does not begin with
a separator — correct on every system musl was written for, and not on one that
writes a volume first — and one walks the program headers of an image in a format
two of the three targets do not have.

Four names have no C in musl to fall back on — `setjmp`, `longjmp`,
`sigsetjmp` and the cancellable system-call sequence. The first three are
supplied by one assembly file that assembles for every object format; the
fourth is a C function, because what it did that C could not do was issue a
system call from a known instruction, and there is no such instruction here.

The measurement that found those four was made by reading the contents of
musl's placeholder sources rather than checking that they exist. musl ships a
zero-byte `.c` for each so that its build system does not fail, so a count of
files answers "all thirty have a C version" and a count of bytes answers
"twenty-six do".

## What a program gets, and what it does not

Everything a C program ordinarily uses works, and `examples/posix` asserts it:
formatted output, the environment, an absolute working directory, files with
creation, positioning, truncation, enquiry and removal, buffered streams,
appending, directories with enumeration, renaming, both clocks, execution
contexts with contended mutexes, allocation and reallocation, and starting
another program and awaiting it.

The following are absent, and each is refused rather than quietly accepted,
because a facility that reports success and does nothing is the one kind of
answer that leaves a program wrong without telling it.

| Absent | What a program observes | Why |
| --- | --- | --- |
| signal handlers | `sigaction` reports `ENOSYS` for any handler other than the default or ignore | openkal has no asynchronous delivery. A handler that was accepted and could never run would be silently wrong; masking, which has nothing to mask, succeeds. |
| memory protection | `mprotect` reports `ENOSYS` | openkal has no operation upon a mapping's protection. musl asks for a guard page below a thread's stack and proceeds without one when told this, so the honest answer is also the one it is prepared for. |
| `fork` | `ENOSYS` | openkal has no operation that duplicates the calling image, deliberately: clause 7.1. `posix_spawn` works, and it is what portable programs use. |
| pipes | `ENOSYS` | openkal has no operation that creates one. |
| readiness | `poll`, `select` report `ENOSYS` | `openkal.event` is reserved and unspecified. |
| symbolic links | `readlink` reports `EINVAL` for a name that is not one and `ENOSYS` for one that is | openkal reserves the operations upon links to an interface it has not defined. |
| ownership and permission bits | `chmod` and `chown` report `ENOSYS`; `stat` reports a mode assembled from what openkal knows | openkal reports what a name refers to and whether it may be written, which is what a capability-based environment can report. |
| entropy | `getrandom` reports `ENOSYS` | openkal has no source of one, and this port does not invent one. The allocator's cookie and the stack canary are derived from the clock and from an address; neither is a security property here. |

Two further boundaries are properties of the arrangement rather than omissions.

**Names that ascend are resolved here, not by the environment.** openkal refuses
a name containing `..`, because a program able to ascend from the directory it
was given would not be confined by having been given it. `/a/b/../c` is
therefore reduced to `/a/c` before openkal sees it, which is what the program
means in every case except one that passes through a symbolic link.

**The tables are bounded.** A program may hold 1024 descriptors and 512 open
descriptions; beyond that it is told so. Allocating the tables instead would
place them on the allocator, and the allocator obtains its memory through them.

## Where POSIX is rebuilt

Two structures live in `port/` that the specification forbids an
*implementation* to have: a table of descriptors, and a resolver of names.

That is the arrangement rather than a violation of it. Clause 7.1 excludes them
from an implementation because an implementation exists once per environment,
and a table required of every implementation is a compatibility layer written
as many times as there are environments. Here there is one of each, above the
boundary, shared by every environment — which is what the specification's
decomposition is for.

The measurement the specification asks for is the size of what exists only to
bridge a difference in shape:

| | lines |
| --- | --- |
| the system-call correspondence (`okm_syscall.c`) | 923 |
| descriptors and name resolution (`okm_fd.c`) | 434 |
| startup and the thread pointer (`okm_start.c`) | 241 |
| `setjmp` and its relatives (`okm_setjmp.S`) | 223 |
| execution contexts and the suspension primitive (`okm_thread.c`) | 202 |
| starting another program (`okm_spawn.c`) | 163 |
| where per-context state is kept (`okm_context.c`) | 128 |
| what two object formats do not provide (`okm_format.c`) | 105 |
| how a second name is made (`port/include/features.h`) | 133 |
| mapping, the working directory, the two architecture seams | 241 |
| **total** | **2793** |

Against 1345 musl sources compiled unmodified. The ratio is the measurement: if
openkal's decomposition were wrong, the port layer would be where the difference
in shape accumulated, and it would grow rather than the library above it
shrinking. Between the first system and the third it grew by 800 lines, and
every one of those is about an object format or a naming convention rather than
about a kernel — which is the shape of the result rather than a qualification of
it.

## Verification

`examples/wordcount` is an ordinary POSIX program whose source mentions nothing
of any of this. Its three counts are compared against the system's own `wc`,
which is an oracle this package did not produce; a program that merely produced
output would prove only that it produced output.

`examples/posix` makes 32 assertions, each written so that it can fail. The
three conditions `kal_fs_open` exists to express are observed by their effect
rather than by a return value: a truncation that did not happen leaves a longer
file, an exclusion that did not happen succeeds, and an append that did not
happen overwrites.

## Three object formats, and what each cost

| | |
| --- | --- |
| ELF | the reference case. musl gives almost every public name to a definition through a weak alias, and this format has weak aliases |
| PE | measured, with both toolchains: a weak symbol is **not a definition** there. The 289 aliases are made strongly, and the 46 names musl provides as placeholders for another source to replace are not made at all — which is decided by the name of the placeholder's target, so no site of musl's is edited |
| Mach-O | the compiler refuses the construct outright: *aliases are not supported on darwin*. The assembler's own directive makes the name instead, and the same placeholder list applies. Two further properties of that format are recorded in `musl/PATCHES.md`: a definition nothing else in the unit refers to is deleted before the assembler sees the name, and a section with no content has no atom and takes its symbols with it |

## Three operations expressed differently

Each is recorded in `musl/PATCHES.md` rather than left to be discovered.

**`fork` is absent and stays absent.** Duplicating a running image is not
something every environment can produce, and clause 3.1 of the specification
declines to simulate what cannot be supplied. `posix_spawn` — and therefore
`system` and `popen` — are supplied, because musl builds them on starting a
program.

**`execve` is starting a program, waiting for it, and ending with its status.** A
caller cannot distinguish that through this library: the same program runs, with
the same arguments, on the same streams, and the same status reaches whoever
waits. There are two images where a system with the operation would have one. It
is what every environment without the operation does, and two of the three
beneath openkal are such environments.

**A program named without a suffix** is tried with one environment's suffix
second, which is what every C library for that environment does. It is here
rather than beneath because openkal is deliberately literal about names: it
passes on the name it was given and does not know that a program is a kind of
file.

## What runs above it

| | |
| --- | --- |
| `examples/posix` | 32 observations, each written so that it can fail |
| `examples/wordcount` | the same three counts as the system's own `wc` |
| [`mcpplibs/sbase`](https://github.com/mcpplibs/sbase) | all 97 suckless base utilities, sources unmodified, 50 comparisons against the system's own tools |

The third is the one the specification's claim is actually tested by. sbase has
no Windows support and does not build on macOS as it stands — three of its tools
include `<sys/sysmacros.h>`, which is a header of the Linux C libraries, and four
use `st_mtim` where that system's C library has `st_mtimespec`. Neither obstacle
is in a kernel; both are the C library, and this package removes them by being
present rather than by being adapted to.

## Relation to `openkal-libc`

This package was `openkal-libc` up to version 0.2.0. That package was a probe of
243 lines exporting 14 names — it did the two things the specification places
outside itself, and it was not a C library. It has no separate existence now:
its path resolution is the resolver described above, and its mutex is musl's.

## Continuous integration

| system | toolchain |
| --- | --- |
| Linux | gcc, llvm |
| macOS | llvm |
| Windows | gcc, producing PE |

The third toolchain mcpp offers is absent, and the reason is a property of the
sources rather than a gap: musl's four remaining assembly definitions are in an
object format that toolchain does not assemble, so the question it would answer
is not one this package can ask.

## License

The port is Apache-2.0. The vendored musl sources under `musl/` are MIT, and
`musl/COPYRIGHT` is unchanged.
