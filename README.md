# openkal-musl

musl 1.2.5 redirected onto [openkal](https://github.com/mcpplibs/openkal).

The specification claims that porting one C library causes the software above it
to run on every implementation. This package is that port, and it exists so that
the claim can be checked rather than repeated.

```toml
[dependencies]
openkal-musl = "0.10.0"
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
`long`, which is not a machine word on one of the three targets — and **ten
replaced sources**. Five of the ten are replaced for the same reason: each
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

The tenth is replaced because the fifth was. `src/process/posix_spawnp.c` does
not search a PATH itself: it hands `__execvpe` to `posix_spawn` to be run **in
the duplicate**, and this port has no duplicate to run it in, so the field was
read by nobody and a name without a separator was taken as a path. It reported
success for a program it had not started.

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
| ~~symbolic links~~ | **answered since 0.7.0** — `symlink`, `readlink`, and `stat`/`lstat` telling the two questions apart | openkal 0.9 carries `kal_fs_link_create` and `kal_fs_link_read` as operations of `openkal.fs`, and `kal_fs_props` takes the directory, so this port asks whether the volume has such nodes before it uses them. Where it does not, the refusal is what the enquiry already said. |
| permission bits | `chmod` reports `ENOSYS`; `stat` reports a mode assembled from what openkal knows | `kal_node_info` carries `writable` — one boolean, not a mode word — and `kal_fs_open` takes flags rather than a mode. Mapping the owner-write bit onto it would make `chmod(0600)` succeed and `stat` report something else, which is the shape this port exists to avoid. |
| the identity of a node | `st_dev` and `st_ino` are the implementation's answer where it has one, and **zero for both where it has none** | ⚠️ They were the constants 0 and 1, so every file compared equal to every other: `std::filesystem::equivalent` on two separately created files answered `true` **with no error**. openkal 0.9 carries an identity and reports whether it knows one; a caller must not read two zeroes as sameness, which is why nothing is invented for an implementation that cannot distinguish nodes. |
| ownership | `chown` reports `ENOSYS`; `stat` reports 1000 for both | as the row above: a capability-oriented environment has no principal for an owner to name. |
| a mode given at creation | `open(…, O_CREAT, 0600)` and `mkdir(path, 0700)` **succeed** and `stat` afterwards reports 0666 and 0777 | the row above, in the one place where it does not read as a refusal. openkal opens a file for a purpose and not for an audience, so the argument has nowhere to go. Refusing every mode but the one `stat` will report would refuse nearly every program; what a caller can rely on instead is stated below. |
| entropy | `getrandom` reports `ENOSYS` where the backend declines `openkal.random` | openkal has no source of one to require, and this port does not invent one. The allocator's cookie and the stack canary are derived from the clock and from an address; neither is a security property here. |
| a signal delivered anywhere | `raise` and `kill` perform a signal's **default action** and nothing else: terminating signals end the program, ignored ones succeed, stopping ones report `ENOSYS`, and musl's own three (32, 33, 34) are refused, so `pthread_cancel` reports `ENOSYS` | there is no delivery, so there is no handler to reach; what remains of a signal is what it does when no handler exists. `abort` reaches `kal_abort`, which raises the signal on Linux and ends with a distinguished status elsewhere — a parent can tell an abnormal end from an ordinary one on every system. |
| an immediate answer about a started program | `waitpid(…, WNOHANG)` returns without the program having finished, but may wait up to one polling interval of the implementation beneath (one millisecond on Linux) | `kal_timeout_wait_process` takes a bound and openkal spells "no bound" as zero, so a caller that does not want to wait asks for the smallest bound there is. An environment rounds a bound up to what its clock can distinguish; a bound shorter than the clock is a promise no environment can keep. |
| closing a standard stream in a program being started | `posix_spawn_file_actions_addclose(&fa, 0…2)` makes the spawn report `ENOSYS`; above position two it is performed, because nothing there is inherited | openkal has no value meaning "no stream", and the value that looks like one — zero — means the opposite: the stream the caller has. Accepting the action and not performing it would hand a program the standard input its caller had just taken away. |
| starting a program upon a stream whose handle is zero | a caller that redirects its **output** onto its own standard input and then starts a program gets `ENOSYS` | `kal_spawn_streams` reserves zero for inheritance and `kal_stream` reserves nothing, so an implementation whose streams are the environment's own descriptors hands out zero for standard input. The two agree at position zero and cannot be told apart anywhere else. Reported upstream; refused here rather than answered wrongly. |
| ~~a version a program can read~~ | **answered since 0.9.0.** `uname`'s `release` field is this package's version, and `OPENKAL_MUSL_TRACE=enosys` names it on the error stream once per process before the program runs | It was the string literal `0.5.0` through every release after 0.5.0, so a program that asked was not left without an answer -- it was given a false one. ⚠️ It therefore MOVES AT EVERY RELEASE: nothing here or in musl reads it (`gethostname` and `getdomainname` are musl's only consumers of `uname` and both read `nodename`), but a program above it that compares the field against a fixed string will see it change. `sysname` is `openkal` and not `Linux`, so nothing can have been reading it as a kernel version. |
| ~~**setting** the modification time of a directory~~ | **answered since 0.10.0 where the implementation can open a directory**, which Linux and macOS can and Windows cannot — its `kal_fs_open` names `FILE_NON_DIRECTORY_FILE`, so `utimensat` on a directory is still refused there. **Reading** it was never affected and is correct everywhere | the port used to ask for `KAL_OPEN_READ \| KAL_OPEN_WRITE` unconditionally, which a directory refuses; it now asks what the name refers to and opens a directory for reading only. ⚠️ **That is outside what `fs.h` states** — the interface requires `KAL_OPEN_WRITE` for `kal_fs_set_modified` and names `kal_fs_open_dir`, which yields a `kal_dir`, as the way to open a directory, while `kal_fs_set_modified` has no `kal_dir` form. So there is no stated route to a directory's time at all; one has been asked for. A file still asks for exactly what the interface requires. Note that libc++ gives both overloads of `last_write_time` the same name in the message it throws, so a failure did not say which of the two had failed — and the one that worked was the one a consumer reported as broken. |
| ~~a lock on a file~~ | **`fcntl(F_SETLK)`, `F_SETLKW` and `F_GETLK` report `ENOSYS` since 0.10.0.** ⚠️ They used to answer 0 and do nothing, so **two programs took one exclusive lock and both were told they had it**; `F_GETLK` left the caller's word untouched, which reads as "somebody holds this" — for ever, so a loop waiting for a lock to be released never left it. `flock` has no case and reports `ENOSYS` too | openkal has no locking operation. ⭐ **Unlike the permission row, this refusal is temporary**: `fcntl(F_SETLK)` on Linux and macOS and `LockFileEx` on Windows all exist and all take a byte range, so every environment beneath openkal can perform it — what is missing is a word in the specification, and one has been asked for (a `kal_fs_lock` beside a `kal_fs_props` position, admitted on exactly the grounds the link operations were). It cannot be composed here meanwhile: a lock built from `KAL_OPEN_EXCLUSIVE` and a name beside the file is released by nobody when its holder dies, so a program that ended abnormally while holding one would be locked out of its own file for ever. |
| whether a file may be executed | `access(path, X_OK)` answers **yes for anything that exists**, and starting a name that exists and cannot be run still ends the caller with 127 | `kal_node_info` carries `writable` and no other permission, so "it is there" is the whole of what this port can answer. The two halves are the same gap: the enquiry cannot tell, and neither can the check `posix_spawn` makes before starting. openkal-linux knows — its own duplicate is the thing that fails — and has been asked to report it. |
| descriptors above 2 crossing into a started program | a started program receives standard input, output and error and **nothing else**; a non-close-on-exec descriptor 4 is not there, and `fcntl(F_SETFD, 0)` upon one therefore changes nothing | `kal_spawn_streams` has exactly three positions and openkal has no general form for placing a stream at position *n*. `posix_spawn_file_actions_adddup2` above position two is already refused rather than accepted, so the two agree; only implicit inheritance is lost. A general form has been asked for. |
| how many processors there are | `sched_getaffinity` reports `ENOSYS`, so `std::thread::hardware_concurrency()` and `sysconf(_SC_NPROCESSORS_ONLN)` answer **1** | ⚠️ this one is silent: a program sizing a pool of workers gets one worker and no error. `openkal.task` says whether contexts run in parallel (`KAL_TASK_PROP_PARALLEL`) and not how many can; an enquiry has been asked for beside that word. |
| volume capacity, hard links, named pipes | `statvfs` (`std::filesystem::space`), `link` (`create_hard_link`), `mkfifo` and `socketpair` report `ENOSYS` | openkal has no operation for any of them. `kal_fs_link_create` makes a node whose content is a name — a symbolic link — and there is no hard link; `kal_process_channel` is a pipe in one direction, so a bidirectional pair is not one of them. Each is a loud absence rather than a wrong answer, which is why none is composed here. |
| an alternate signal stack | `sigaltstack` reports `ENOSYS` since 0.10.0 | it used to report success and install nothing, and the enquiry that would have caught it answered 0 with a zeroed record. There are no signals here, so there is nothing for such a stack to be. |

**⭐ What carries confinement here, since a mode word does not.** A program that
writes "only I may read this" as a mode is stating it in a vocabulary this
environment does not have. What it does have is stronger and is not the
program's to weaken: a program reaches only the directories the environment
supplied it, and `port/src/okm_fd.c` states the rule — confinement is a property
of what was supplied, not of the program's cooperation. A caller with that
requirement expresses it by being started with fewer directories.

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

**`O_NOFOLLOW` is answered by an enquiry, not by an opening.** openkal states
that opening resolves and offers no form that declines to, deliberately: a
program that opens a link in order to read its bytes is asking what
`kal_fs_link_read` answers. So this port asks `kal_fs_info` with
`KAL_FS_NO_RESOLVE` first and reports `ELOOP` when the name is a link, which is
what POSIX says and what a caller passing the flag is distinguishing. Answering
`ENOENT` instead — which is what resolving a link to an absent target produces —
is a different answer to a different question, and libc++'s `remove_all` reads it
as "the entry has already gone" and leaves the tree standing.

**The tables are bounded.** A program may hold 1024 descriptors and 512 open
descriptions; beyond that it is told so. Allocating the tables instead would
place them on the allocator, and the allocator obtains its memory through them.

⚠️ **And a program may have started 256 programs it has not waited for.** An
entry is taken when a program is started and released when it is waited for,
which is what a process table is; a program that never waits holds entries for
ever, and the next start reports `EAGAIN` — which is what POSIX says `fork`
does when the table is full. This bound is stated here because it was not, and
a caller that met it saw a failure on an operation with no evident relation to
the ones that caused it: measured, the sixty-fifth `posix_spawn` of a program
that waited for none, and of one that polled each once with `WNOHANG` and did
not come back.

## Asking which operation was missing

`ENOSYS` says that a facility is not here. It does not say which one, and until
0.6.0 the only way to find out was to read `port/src/okm_syscall.c` — which is
not a thing a consumer of a C library should have to do, and two rounds of
[openkal-linux#13](https://github.com/mcpplibs/openkal-linux/issues/13) were
spent on exactly that question.

    OPENKAL_MUSL_TRACE=enosys ./your-program

Each operation the dispatcher has no case for is then named on the standard
error stream, **once**, whatever the number of attempts:

    openkal-musl 0.10.0
    openkal-musl: no operation for system call 266

**The first line is the version, and it is printed whether or not anything is
missing.** That is the whole reason it exists. Before 0.9.0 a run in which
nothing was refused printed nothing at all, and three situations were then
indistinguishable: the version is right and no operation is absent, the variable
did not take effect, or this is not the binary the reader thinks it is. Two
rounds of [openkal-linux#13](https://github.com/mcpplibs/openkal-linux/issues/13)
were answered against the wrong version because of it.

A report pasted into an issue therefore carries its own provenance, and one
process contributes one such line — a program that starts another produces one
for each, and they must agree.

Four properties, each of them asserted in continuous integration because each
of the corresponding failures is quiet:

- **Nothing is reported unless the variable is set.** A diagnostic that appears
  by itself is one every program above this library has to explain to its users.
- **Once per operation.** A program that retries in a loop would otherwise bury
  the report in copies of itself, and a reader counting lines would conclude it
  happened once.
- **Only the operations that have no case.** `mprotect` and `rt_sigreturn`
  answer `ENOSYS` from cases of their own, each a decision with a reason
  recorded beside it. Reporting those would name a facility as missing that this
  port deliberately does not have, which is a different sentence.
- **The version named is the one in `mcpp.toml`.** It is read from the manifest
  by `build.mcpp` rather than written out a second time, and the workflow
  compares the printed line against the manifest. A version stated in two places
  agrees until one of them is edited.

The report is written to the stream directly rather than through this library's
own output, because what failed may be the operation that output was about to
perform.

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
| the system-call correspondence (`okm_syscall.c`) | 1703 |
| sockets and datagrams (`okm_net.c`) | 788 |
| descriptors and name resolution (`okm_fd.c`) | 512 |
| `setjmp` and its relatives (`okm_setjmp.S`) | 316 |
| how a second name is made (`port/include/features.h`) | 288 |
| startup and the thread pointer (`okm_start.c`) | 282 |
| readiness and bounded transfer (`okm_poll.c`) | 256 |
| execution contexts and the suspension primitive (`okm_thread.c`) | 203 |
| starting another program (`okm_spawn.c`) | 372 |
| where per-context state is kept (`okm_context.c`) | 162 |
| duplicating the calling image (`okm_fork.c`) | 166 |
| what two object formats do not provide (`okm_format.c`) | 105 |
| mapping, the working directory, program headers, the architecture seams, the one file each of two object formats needs | 398 |
| **total** | **5551** |

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

**`execve` is starting a program, waiting for it, and ending with its status.**
It is what every environment without the operation does, and two of the three
beneath openkal are such environments.

⚠️ **This paragraph used to add that a caller cannot distinguish it. A caller
can, and saying otherwise is what kept anyone from looking.** Three differences
are known and `musl/PATCHES.md` states each: a program that cannot be started
(**answered since 0.10.0** — the name is asked about first, so `execvp` can
search a PATH), a `kill` that reaches the waiting copy rather than the program
(**not answered**; use `posix_spawn`, `system` or `popen` where a caller needs
to stop what it started), and the identifier the started program reports.

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
| `examples/subprocess` | 22 observations: another program started five ways, where its output went in each of them, how an abnormal end is reported, and three refusals checked as refusals |
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
