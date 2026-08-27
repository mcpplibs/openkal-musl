# openkal-musl

musl 1.2.5 redirected onto [openkal](https://github.com/mcpplibs/openkal).

The specification claims that porting one C library causes the software above it
to run on every implementation. This package is that port, and it exists so that
the claim can be checked rather than repeated.

```toml
[dependencies]
openkal-musl = "0.5.0"
```

It names no implementation and no platform: a C library is the one consumer that
must know which implementation it needs, so it declares that itself.

One further line is required, and the reason is worth stating because it is not
about openkal:

```toml
[build]
cxx_runtime = "host-coupled"
```

The build tool decides whether to embed a C++ runtime into the program, and on
one of the three systems its answer is to embed one and to run that runtime's
initialiser **first** --- before anything else in the image, including the entry
point. A program above this package carries no other runtime, so the C library
has not started at that moment; the embedded initialiser reaches it through a
guarded static and a mutex and finds nothing there, and the report names the
dereference rather than the ordering. `host-coupled' says that the build tool
embeds nothing: the system's own C++ runtime is named instead, and its
initialisers run inside it, on the facilities it was built against.

The decision belongs to the program because the build tool reads it from the
program. A dependency that declared it would be declaring it for itself.

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

Sockets, datagrams, readiness and the duplication of the calling image were in
the table below until openkal 0.8, and they are not there now. What changed is
the specification: `openkal.net`, `openkal.datagram`, `openkal.timeout` and
`openkal.space` gave this port the atoms it had been missing, and each of the
four is now composed above them rather than refused. `examples/net` and
`examples/subprocess` assert them, written against POSIX and naming no openkal
symbol.

**⚠️ Every one of those four depends on what is beneath.** Clause 3 permits an
implementation to provide an interface in whole or not at all, and the four
interfaces are optional. Where a backend declines one, the routes that use it
report `ENOSYS` — the port takes a weak reference and tests it before calling,
so a program that never opens a socket links and runs above a backend that has
no network at all.

The following are absent, and each is refused rather than quietly accepted,
because a facility that reports success and does nothing is the one kind of
answer that leaves a program wrong without telling it.

| Absent | What a program observes | Why |
| --- | --- | --- |
| signal handlers | `sigaction` reports `ENOSYS` for any handler other than the default or ignore | openkal has no asynchronous delivery. A handler that was accepted and could never run would be silently wrong; masking, which has nothing to mask, succeeds. |
| memory protection | `mprotect` reports `ENOSYS` | openkal has no operation upon a mapping's protection. musl asks for a guard page below a thread's stack and proceeds without one when told this, so the honest answer is also the one it is prepared for. |
| out-of-band data | `MSG_OOB`, `MSG_PEEK`, and `POLLPRI` are never reported and `recv` refuses the flags | openkal's transfer operations move bytes and have no second channel and no non-destructive read. |
| readiness *sets* | `epoll` is not built at all, so the link names it | a set held by the environment is a facility of one kernel rather than a capability. `poll` and `select` ask each descriptor in turn, which is what an interface without a set permits. |
| symbolic links | `symlink` reports `ENOSYS`; `readlink` reports `EINVAL` for a name that is not one and `ENOSYS` for one that is | `SURFACE.txt` has no operation that creates or reads a link. It *does* have `kal_node_link` and `KAL_FS_PROP_LINKS`, so an implementation can report a link it encounters and cannot make one; the asymmetry is the specification's and is recorded rather than worked around. |
| ownership and permission bits | `chmod` and `chown` report `ENOSYS`; `stat` reports a mode assembled from what openkal knows | `kal_node_info` carries `writable` — one boolean, not a mode word — and `kal_fs_open_file` takes flags rather than a mode. Mapping the owner-write bit onto it would make `chmod(0600)` succeed and `stat` report something else, which is the shape this port exists to avoid. |
| entropy | `getrandom` reports `ENOSYS` where the backend declines `openkal.random` | openkal has no source of one to require, and this port does not invent one. The allocator's cookie and the stack canary are derived from the clock and from an address; neither is a security property here. |

**⭐ The permission row is a decision and not an omission.** The alternative was
to ask the specification for a permission operation. It was declined: a FAT
volume, a UEFI system partition and a Windows access-control list do not share a
model, so an operation upon permissions is one that some resources of the
interface can never satisfy — which is what clause 6.4 excludes. Refusing here
and stating why is the answer; `.agents/docs` in the specification's repository
records the reasoning.

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
| the system-call correspondence (`okm_syscall.c`) | 1496 |
| sockets and datagrams (`okm_net.c`) | 788 |
| descriptors and name resolution (`okm_fd.c`) | 463 |
| `setjmp` and its relatives (`okm_setjmp.S`) | 316 |
| how a second name is made (`port/include/features.h`) | 288 |
| startup and the thread pointer (`okm_start.c`) | 282 |
| readiness and bounded transfer (`okm_poll.c`) | 256 |
| execution contexts and the suspension primitive (`okm_thread.c`) | 203 |
| starting another program (`okm_spawn.c`) | 164 |
| where per-context state is kept (`okm_context.c`) | 162 |
| duplicating the calling image (`okm_fork.c`) | 166 |
| what two object formats do not provide (`okm_format.c`) | 105 |
| mapping, the working directory, program headers, the architecture seams, the one file each of two object formats needs | 398 |
| **total** | **5087** |

Against 1345 musl sources compiled unmodified. The ratio is the measurement: if
openkal's decomposition were wrong, the port layer would be where the difference
in shape accumulated, and it would grow rather than the library above it
shrinking. Between the first system and the third it grew by 800 lines, and
every one of those is about an object format or a naming convention rather than
about a kernel — which is the shape of the result rather than a qualification of
it.

**⚠️ It grew by 2294 more when the socket, datagram, readiness and image-copying
routes were added, and that number deserves a reading rather than a footnote.**
Roughly half of it is comment; of the code, the largest single piece is the
state machine in `okm_net.c`, and what that machine bridges is one difference in
shape: BSD makes a socket first and decides what it is afterwards, and openkal
has no object between "nothing" and "a connection". The rest is the read-ahead
that answers a readiness enquiry, which exists because openkal deliberately has
no operation reporting whether a transfer would proceed (clause 6.3).

Both are shape and neither is environment. They are written once here and are
what every implementation of openkal is spared — which is the property the ratio
was measuring in the first place, and the growth does not change its direction.

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

`examples/net` makes 35 over a listener, a connection and a pair of datagram
endpoints, all on the loopback address, and asks nothing of the network beyond
the machine it runs on.

`examples/subprocess` starts another program three ways — `fork`, `system`,
`popen`.

**⭐ Those two say what they expect on the command line rather than inferring
it.** `--fork` requires that duplicating the calling image work; `--no-fork`
requires that it be *refused*. An environment whose backend declines
`openkal.space` is not a failure, and an environment expected to provide it that
quietly does not IS one — a probe that accepted either answer could not tell
them apart, and the interesting failure is exactly the one it could not see.

`examples/identifiers` compiles rather than runs. It declares `hidden`, `weak`
and `weak_alias` as ordinary identifiers, which a program above this package
could not do until the internal overlay's macros were scoped to the overlay
(`mcpplibs/openkal-musl#13`). If any of the three is a macro again, the file does
not compile.

## Three object formats, and what each cost

| | |
| --- | --- |
| ELF | the reference case. musl gives almost every public name to a definition through a weak alias, and this format has weak aliases |
| PE | measured, with both toolchains: a weak symbol is **not a definition** there. The 289 aliases are made strongly, and the 46 names musl provides as placeholders for another source to replace are not made at all — which is decided by the name of the placeholder's target, so no site of musl's is edited |
| Mach-O | the compiler refuses the construct outright: *aliases are not supported on darwin*. The assembler's own directive makes the name instead, and the same placeholder list applies. Two further properties of that format are recorded in `musl/PATCHES.md`: a definition nothing else in the unit refers to is deleted before the assembler sees the name, and a section with no content has no atom and takes its symbols with it |

## Three operations expressed differently

Each is recorded in `musl/PATCHES.md` rather than left to be discovered.

**`fork` is composed here rather than required beneath.** `openkal.space` starts
a context in a *copy of the calling address space*, and stops there: the started
context begins at a function the caller names, not at the instruction the caller
was executing, because that is what can be stated in a C application binary
interface at all. `fork` returns twice, so the second half is this port's:
`setjmp` before the call, `longjmp` in the copy. The specification's own
`space.h` describes that composition and says in terms that it belongs above the
line, which is where it now is (`port/src/okm_fork.c`).

⚠️ **An earlier version of this file said `fork` was absent and would stay
absent**, on the reading that clause 7.1 declines to duplicate an address space
*and its execution state*. Half of that is right: the clause declines the
**pair**. `openkal.space` supplies the first half by itself, and what was
missing was never an atom.

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
| `examples/net` | 35 over sockets, datagrams and readiness, on the loopback address |
| `examples/subprocess` | another program started three ways, and a refusal checked as one |
| `examples/identifiers` | three names a program above this library may use, asserted by compiling |
| `examples/wordcount` | the same three counts as the system's own `wc` |
| [`mcpplibs/sbase`](https://github.com/mcpplibs/sbase) | all 97 suckless base utilities, sources unmodified, 50 comparisons against the system's own tools |

The third is the one the specification's claim is actually tested by. sbase has
no Windows support and does not build on macOS as it stands — three of its tools
include `<sys/sysmacros.h>`, which is a header of the Linux C libraries, and four
use `st_mtim` where that system's C library has `st_mtimespec`. Neither obstacle
is in a kernel; both are the C library, and this package removes them by being
present rather than by being adapted to.

## Relation to `openkal-libc`

This package was `openkal-libc` up to version 0.2.0, and the version line
continues rather than restarting: 0.1.0 and 0.2.0 name the earlier package in
this repository's own tags, so a version that restarted at 0.1.0 would give one
tag two meanings. The name changed at 0.3.0 and the numbers do not go backwards.
 That package was a probe of
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
