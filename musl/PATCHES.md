# What was changed in musl, and why

musl 1.2.5 is vendored here unmodified except for the four lines below, and the
list is exhaustive: `git log -p -- musl/` shows every one of them. Each is
recorded with the reason, because a port that silently edits the library it
ports is a port nobody can check.

Five further sources are *excluded* rather than changed, and those are listed in
the package manifest with the reason beside each. Excluding a source and
supplying a replacement is visible in one place; editing a source is not, which
is why the two are treated differently.

## `src/internal/syscall.h`, one declaration

```diff
-hidden long __syscall_ret(unsigned long),
+hidden syscall_arg_t __syscall_ret(syscall_uret_t),
```

Every system call's result passes through this function. musl declares it as a
`long` because on every architecture musl supports, a `long` holds a pointer —
`mmap` returns one through it, and `lseek` returns a sixty-four-bit offset
through it.

Windows is LLP64: a `long` is thirty-two bits and a pointer is sixty-four. Left
as it was, every mapping would lose the upper half of its address and every
offset beyond two gigabytes would be truncated, and both would happen silently.

`syscall_arg_t` is the type musl already provides for exactly this purpose:
`syscall_arch.h` defines it per architecture, and two of musl's own targets
already widen it because their arguments are wider than a `long`. On every
target musl supports, the changed line says what the original said.

## `src/stdio/vfwscanf.c`, `src/stdlib/wcstol.c`, `src/stdlib/wcstod.c`

```diff
-set = L"";
+set = (const wchar_t[]){0};
```

musl's `wchar_t` is thirty-two bits on every architecture it supports, and this
environment's wide-character literal is sixteen. Three of musl's sources write a
wide literal, and in each the literal is written as an array instead so that it
has musl's type rather than the environment's.

This does not make the two agree, and it is not intended to. A program above
this library that writes `L"..."` will fail to compile on Windows, and that is
the outcome preferred: the alternative — narrowing musl's `wchar_t` to match the
environment — would make every code point above U+FFFF convert to the wrong
value with nothing reporting it.

## Not changed: the five excluded sources

`src/env/__libc_start_main.c`, `src/env/__init_tls.c`,
`src/thread/__set_thread_area.c`, `src/thread/clone.c`,
`src/process/posix_spawn.c`, and on every target `src/mman/mmap.c` and
`src/internal/syscall_ret.c`.

Each reads the shape of one environment directly rather than asking a kernel for
something, and each is replaced by a source in `port/` that asks openkal
instead. They are excluded in the manifest, where the exclusion and its reason
are visible together.

## Two more, found by running rather than by reading

### `src/thread/__syscall_cp.c` and `src/thread/pthread_cancel.c`

The same widening as `syscall.h`, applied to the cancellable form of the same
function. `__syscall_cp` carries a system call's result and was declared to
carry it in a `long`.

## The sources this port replaces, and why each

Eleven, and the list in the manifest carries the same reasons. Five read the shape
of one environment directly. Two carry a machine word through a variable
declared `long`. Two more were found only by running the result. And one is
replaced because another already was:

`src/process/posix_spawnp.c` does not search a PATH. It stores `__execvpe` in
the attributes and lets `posix_spawn` call it **in the duplicate** instead of
`execve`, so the search happens inside a program that has already been started.
This port replaced `posix_spawn` and has no duplicate to run it in, so the field
was read by nobody: a name without a separator was taken as a path relative to
the working directory, and `posix_spawnp("sh", …)` started `./sh`, failed, and
**reported success**. `port/src/okm_spawn.c` performs the search, one
`__posix_spawn` per entry, by musl's own rules — and `__posix_spawn` now refuses
an attribute function it does not recognise rather than ignoring one.

⚠️ **Including musl's entry separator, which is a colon on every target and is
the wrong one for exactly one of them.** One environment separates its own PATH
with a semicolon and begins each entry with a volume letter and a colon, so
reading that PATH on a colon produces entries that are not names. It is still a
colon here because `execvp` — which this port does **not** replace — splits on
one in musl's own source, and having the two ways of searching for one program
disagree with each other is worse for a caller than having both wrong the same
way. Nothing on that target searches a PATH today: its CI row declares no shell.

`src/mman/mmap.c` returns a pointer through a `long`. It is replaced rather than
patched because the replacement is also better where a `long` does hold a
pointer: the value never becomes an integer at all.

`src/unistd/getcwd.c` refuses an answer that does not begin with a separator.
That is correct on every system musl was written for and is not correct on one
that writes a volume first, and openkal does not say which a system does.

⚠️⚠️ `src/fcntl/fcntl.c` is the THIRD of that kind, and it survived three releases
of a port that already names the kind twice.

It reads its variable argument as an `unsigned long`:

    unsigned long arg;
    arg = va_arg(ap, unsigned long);
    case F_SETLK: return syscall(SYS_fcntl, fd, cmd, (void *)arg);

which holds a pointer on every system musl was written for and thirty-two bits
on one this port builds for. A caller passing a `struct flock *` had the top half
of it discarded **before this port saw it**.

⭐ **It was unreachable until 0.11.0**, which is why it survived. Every command
this library answered took an integer, or took a pointer it never followed:
`F_SETLK` returned 0 and did nothing, and then reported `ENOSYS`. A truncated
pointer that nothing dereferences is a truncated pointer nothing reports. openkal
0.10 gave this port a real lock, `F_SETLK` began following the pointer, and it
faulted on the first attempt.

⚠️ The register file names the type rather than the symptom:

    page fault on read access to 0x00000000fe2ffec2
    rax:00000000fe2ffec0   rsp:00007ffffe2fc7a0
    movzxw 0x02(%rax), %eax

`rax` is the caller's pointer with its top thirty-two bits gone, and the offset
it faults at — two — is `l_whence`, the first field this port reads.

⚠️ And the vararg TYPE is part of the calling convention rather than a detail:
`va_arg(ap, unsigned long)` and `va_arg(ap, uintptr_t)` read different numbers of
bytes where the two differ, so this is not a cast applied afterwards. Reading it
as the narrower type has already lost the half by then.

## What has no equivalent on one object format

`port/include/features.h` records a measurement rather than a patch: on PE, as
both toolchains implement it, a weak symbol is not a definition. musl gives
almost every public name to a definition through a weak alias, so on that format
the aliases are made strongly, and the 46 names musl provides as placeholders
for another source to replace are not made at all --- which is decided by the
name of the placeholder's target and needs no change to musl.

Two object formats reach that conclusion by different routes. On PE the
construct compiles and produces a record no linker resolves. On Mach-O the
compiler refuses it outright --- "aliases are not supported on darwin" --- so the
second name is made with the assembler's own directive instead:

```c
int base(void) { return 7; }
__asm__(".globl _w\n\t.set _w, _base");
```

which produces `w` as a definition at `base`'s address. That is a strong name
too, so the same placeholder list applies, and `port/src/okm_format.c` supplies
what is then absent on either format.

## Three operations this library expresses differently, and what a caller can tell

Each is recorded here because each is a divergence a reader would otherwise have
to find by testing.

`execve` replaces the running image, and openkal has no such operation: an
environment that cannot replace a running image cannot supply one, and clause
3.1 of the specification declines to simulate what cannot be supplied. This
library expresses it as starting the program, waiting for it, and ending with
the status it ended with. It is the arrangement every environment without the
operation uses, and two of the three beneath openkal are such environments.

⚠️⚠️ **This paragraph used to say that a caller cannot distinguish that. It can,
and the claim is what kept anyone from looking.** Three differences are known,
and the first two were found by a consumer rather than here:

1. **When the program cannot be started.** The replacement happens inside the
   duplicate, so its failure is not the caller's to see; the duplicate ended
   with 127 and `execve` ended the caller with it, instead of returning -1.
   musl's own `execvp` issues one `execve` per PATH entry and needs each to
   return, so a program named without a separator was found only when it
   happened to sit in the first entry. **Answered since 0.10.0**: the name is
   asked about before the program is started, so `ENOENT` and `EACCES` reach
   the caller. What is still not answered is a name that exists and cannot be
   executed — openkal reports no execute permission, so that one still ends the
   caller with 127. Asked of openkal-linux, which knows and does not report it.
2. **`kill` did not reach a program started this way.** After `fork` and
   `execve` there are three images, not two: the copy waits for the program it
   started, and a signal sent to the identifier the parent holds reached the
   waiter. The parent was told the program died on that signal while the program
   ran to completion, unsupervised. Measured, with the host as control:
   identical status words, opposite outcomes.

   ⚠️ **This entry used to end "Not answered here", and it is answered now.**
   openkal had no way to say "this program's lifetime is bound to mine", and
   `kal_process_terminate` was right to terminate only what it was given — so
   what was missing was a word, not a mechanism. openkal 0.10 added
   `kal_process_spawn_bound`, and **since 0.11.0 `execve` asks for it**.
   `posix_spawn` does not and must not: a POSIX child outlives its parent.

   ⚠️ A backend may decline the binding — openkal-macos has no primitive that
   arms it from inside the started image, and openkal-windows has not measured
   its own. There this falls back to the unbound spawn rather than refusing to
   start the program at all, and the divergence is the one this entry used to
   describe. `KAL_PROCESS_PROP_BOUND_LIFETIME` is what a caller asks.
3. **The identifier the started program reports is not the caller's**, because
   there are two images where a system with the operation would have one.

`utimensat` asks the environment for ownership of the file, and this library
asks for write access instead, because openkal's operation is stated on an open
file and one of the three environments decides at the point of opening what may
afterwards be done with it. The consequence is that a file its owner cannot
write cannot have its time set here.

A program is named without a suffix by a caller of this library and with one by
one of the three environments. `port/src/okm_spawn.c` tries the name as given
first and the name with that environment's suffix second, which is what every C
library for it does. It is here rather than beneath because openkal is
deliberately literal about names: it passes on the name it was given and does
not know that a program is a kind of file.

`fork` is **composed here** rather than required beneath, and this paragraph
used to say it was absent and would stay absent. `openkal.space` starts a
context in a *copy of the calling address space* and stops there: the started
context begins at a function the caller names, not at the instruction the caller
was executing, because that is what can be stated in a C application binary
interface at all. `fork` returns twice, so the second half is this port's ---
`setjmp` before the call, `longjmp` in the copy. The specification's own
`space.h` describes that composition and says in terms that it belongs above the
line; `port/src/okm_fork.c` is where it is.

⚠️ **A caller can tell two things.** The copy's per-context identity is not
necessarily the original's --- `kal_task_current()` promises uniqueness among
contexts running at the same moment and says nothing about a copy, and the two
implementations answer differently --- so this port rebinds the copy's slot
before anything reads per-context state. And the copy's view of the table of
started programs is cleared, because POSIX is explicit that a duplicate has no
children and the table lives in this port's own memory.

### The fifteen indirect symbols

Where the definition a second name is made for is in the same translation unit,
the assembler's directive produces an ordinary symbol at that address. Where it
is not, it produces an *indirect* symbol, which is the other thing that
directive is for and which one linker refuses in as many words: `TODO: support
aliasing to symbols of kind 1`.

The measurement: 1154 of musl's translation units compiled for that system
produce fifteen such symbols between them, and every one is a function declared
in a header and defined in another unit.

```
dladdr, dlopen, __dlsym, __dl_invalid_handle, __libc_exit_fini, malloc,
pthread_detach, pthread_equal, pthread_getspecific, pthread_tryjoin_np,
thrd_detach, thrd_equal, tss_get, utmpname, utmpxname
```

The response is one flag on the link line rather than fifteen exceptions in a
macro: the link names that system's own linker, which is the one its object
format was designed alongside and which resolves an indirect symbol.

#### The measurement is toolchain-specific, and the flag's scope is narrower
#### than it reads

Remeasured on 2026-08-22, on Linux, with the open-source toolchain:

| what was measured | result |
| --- | --- |
| every source of this package compiled for `arm64-apple-macos14` (1336 units), `llvm-nm -m` counting indirect symbols | **0** |
| the same for `x86_64-apple-macos14` | **0** |
| the same again with clang 22.1.8 rather than clang 18 | **0** |
| `port/src` (11 units) and `openkal-macos/src` (9 units) added — 1356 objects | **0** |
| `strings` over lld 22.1.8, looking for the refusal | still present: `TODO: support aliasing to symbols of kind ` |
| `ld64.lld` linking all 1356 objects plus a program, **on Linux** | **succeeds** |

The last two rows are not in conflict. The linker's limitation is still there;
what has changed is that the compiler does not produce the construct that
reaches it. The fifteen symbols are a property of **the compiler the measurement
above was made with** — the one this system's own toolchain supplies — and not a
property of this port, of musl, or of the object format.

Two consequences, and both narrow rather than widen what is claimed:

- `--ld-path` in `mcpp.toml` applies to a **native build on that system with that
  system's own compiler**. A cross build from another system does not reach the
  construct and does not need the flag. The comment beside the flag says so.
- A program for that system can therefore be linked **without any file that
  system supplies**. What it needs is recorded under "Two names, and how they
  were counted" below.

#### Two names, and how they were counted

The count was not read out of the sources. Every object of a complete build was
given to the linker **without** `-undefined dynamic_lookup`, and the linker was
asked what remained:

```
_clock_gettime_nsec_np              openkal-macos/src/time.cpp
_pthread_create_from_mach_thread    openkal-macos/src/task.cpp
__DYNAMIC                           src/ldso/dl_iterate_phdr.c
```

The third is not one of them. That source is excluded from this system's build
by `mcpp.toml`, and it appears here only because the probe compiled the whole
tree rather than the configured set. **Reading the sources would not have found
that false positive; the linker did.**

So the whole of what this system supplies to a program built above this package
is two names, and a stub naming them is eight lines of text. `openkal-macos`
carries it as `port/libSystem.tbd`.

## Four more, added with the socket, datagram and readiness routes

**`connect` completes before it returns, even upon a non-blocking descriptor.**
`kal_net_connect` completes or fails; openkal has no form that begins a
connection and reports its outcome later, and clause 6.3 records readiness
notification among the mechanisms considered and not adopted. A port that
returned `EINPROGRESS` would be promising a completion nothing can report: a
caller would then poll for `POLLOUT`, be told ready, ask `SO_ERROR` and be told
zero, all of it invented. What a non-blocking caller loses is the overlap, not
the outcome.

**`POLLOUT` is reported without an enquiry.** openkal has no operation that
reports whether a write would proceed, and a bounded write bounds the *wait* and
not the *transfer* --- `openkal/include/openkal/timeout.h` says so in terms. So
a program that polls for `POLLOUT` to avoid blocking may still block in the
write. Reporting the descriptor as never writable would be worse: nothing would
proceed at all.

**`POLLIN` is answered by taking the input and keeping it.** There is no
non-destructive readiness enquiry to make, so `poll` performs a bounded transfer
and holds what it produced --- one byte from a stream, one connection from a
listener, one message from a datagram endpoint --- for the operation that
follows. A caller can tell in one way: a `read` after a `poll` may report **one
byte** where a kernel would have given it everything that had arrived. A short
read is a result every caller of `read` already handles, and this port answers
`getdents` the same way and for the same reason.

**`O_NONBLOCK` costs one granularity of the environment beneath.** openkal
spells "no bound" as zero, so a non-blocking operation is expressed as the
*smallest* bound rather than as none --- one nanosecond, which an environment
rounds up to what its clock distinguishes (a millisecond on openkal-linux). A
caller that sets `O_NONBLOCK` and finds nothing to read waits that long rather
than not at all. Where the environment provides no `openkal.timeout`,
`O_NONBLOCK` is **refused** rather than accepted and ignored.

## Five more, added with the redirection, signal and bounded-wait routes

These were reported by a consumer rather than found here, as
[openkal-linux#13](https://github.com/mcpplibs/openkal-linux/issues/13), and
each of the five is a place where the specification already carried the atom and
this library had no route to it.

**What a started program's three streams are is decided from the descriptor
table and not from a constant.** `port/src/okm_spawn.c` passed `{0, 0, 0}` and
let a caller's file actions overwrite it. openkal spells inheritance as a handle
of zero, and an implementation reads that as the descriptor the CALLING IMAGE
holds --- which `dup2` cannot change, because it rebinds this library's table and
there is no operation that replaces one of a running program's own streams. So
`dup2(fd, 1)` followed by `posix_spawn`, by `system`, or by `fork` and `execve`
started a program writing to the stream this program had been started with, and
the file the caller had redirected onto stayed empty. What a caller can tell now
is nothing: the redirection is carried across, and the three positions that were
not redirected are still passed as zero so that a backend without
`KAL_PROCESS_PROP_STREAM_PASSING` still starts every program that does not
redirect.

**A file action is applied in the order it was added, and used not to be.**
musl's `posix_spawn_file_actions_add*` PREPEND, so `__actions` names the most
recent; musl's own `posix_spawn` walks to the tail and follows `prev`. This
library followed `next` and applied them in reverse. Invisible until two actions
name one position, which no caller in musl itself produces --- `popen` emits a
single `adddup2`.

**Closing one of the three standard positions in a started program is refused.**
`posix_spawn_file_actions_addclose(&fa, 0…2)` reports `ENOSYS`; above position
two it is performed, because nothing above two is inherited. openkal has no
value meaning "no stream", and zero --- the value that looks like one --- means
the opposite. It used to be accepted and not performed, which handed a program
the standard input its caller had just taken away.

**A signal aimed at this program performs its default action, and `abort` works
because of it.** `SYS_tkill` and `SYS_tgkill` had no case, so musl's `raise`
answered `ENOSYS` and `abort` --- which raises, uninstalls, raises again and then
reaches the line its own comment calls unreachable --- ended in `a_crash()`. On
x86_64 that is `hlt`, which faults outside ring 0 and is delivered as SIGSEGV.
Measured on the host kernel 2026-08-28: a program whose entire body is `hlt`
exits 139 with a core dumped. Every uncaught exception, `assert`,
`std::terminate` and `__stack_chk_fail` over this port therefore reported a
segmentation fault. They now reach `kal_abort`, which raises the signal on
openkal-linux and ends with a distinguished status on the other two.

⚠️ Three numbers are musl's own and must never terminate anything:
`pthread_impl.h` reserves 32, 33 and 34 for the timer thread, cancellation and
`synccall`, each sent with this same call. `pthread_cancel` is
`pthread_kill(t, SIGCANCEL)`, so a table making 33 terminating would end the
program the first time anything cancelled a thread. They are refused, which is
what they already got and what musl already handles.

⚠️ And the target is deliberately not examined. A terminating signal's default
action ends the process rather than the context that was named --- which is true
on Linux too --- so which context a caller aimed at makes no difference.
Comparing the identifier against `kal_task_current()` would have been worse than
useless: a thread created here records `port/src/okm_thread.c`'s own counter
rather than openkal's identity, so the comparison would have failed for every
context but the first and `abort` from a thread would have gone back to `hlt`.

**`waitpid(…, WNOHANG)` returns without waiting, and costs one polling interval
of the environment beneath.** It used to discard its options and block, which is
the one thing that flag exists to prevent. `kal_timeout_wait_process` has been in
the specification since 0.8. openkal spells "no bound" as zero, so this asks for
the smallest bound there is --- the same arrangement `O_NONBLOCK` uses above ---
and openkal-linux, which has no bounded wait for a child and polls, rounds that
up to one millisecond. Where the environment provides no `openkal.timeout`,
`WNOHANG` is refused rather than accepted and ignored.

### And one defect of the criteria rather than of the library

⭐ `tools/run-probe.sh` chose the program to run with `find target … | head -1`.
`target/` accumulates one directory per configuration --- per toolchain, per
target, and per version of a dependency, because the version is part of the
fingerprint --- so after the version moved from 0.5.0 to 0.6.0 the search
answered with the program built BEFORE the change. Two newly added observations
did not appear in the output of a run that reported `-- failures: 0 --`. The
criteria had not failed; they had not run, and nothing said so.

It does not bite in continuous integration, which is why it survived: a fresh
checkout builds one configuration and there is nothing to choose between. It
bites on the machine where the change is being written, which is where a
criterion is trusted most. `tools/one-artifact.sh` now asserts the count before
anything is read, and the steps that run a program call it.

### One thing the specification reserves in one interface and hands out in another

`kal_spawn_streams` reserves a stream handle of **zero** to mean inheritance.
`kal_stream` reserves nothing, and openkal-linux answers `kal_stdin()` with zero
because its streams are the environment's own descriptors. The two agree at
position zero --- placing standard input at standard input and inheriting it are
the same act --- and cannot be told apart anywhere else. A caller that redirects
its OUTPUT onto its own standard input and then starts a program is therefore
asking for something no argument to `kal_process_spawn` can express.

This library **refuses** that spawn rather than passing the zero on, because
passing it on would be read as inheritance and would start the program writing to
the stream the caller had just redirected away from --- the silent wrong answer
this whole set of changes exists to remove. Reported to the specification; it is
not a defect this library can fix, and a refusal is one a caller can act upon.

## Seven more, added with the exec search — and what they have in common

⭐⭐ **Every one of these is an operation that was PRESENT AND ANSWERED WRONGLY,
which is a different failure from an operation that is missing — and it is the
reason none of them was found by the diagnostic added for the last set.**

`OPENKAL_MUSL_TRACE=enosys` reports what reaches the default arm. Not one of the
seven does. They were found by writing `examples/surface`, which asks what an
answer **is** rather than whether a call returned, and comparing every answer
against the host's.

**A lock was granted and never taken.** `fcntl(F_SETLK)`, `F_SETLKW` and
`F_GETLK` answered 0 and did nothing. Measured with the host as control: two
programs took one exclusive lock and both were told they had it. `F_GETLK` was
worse — POSIX writes `F_UNLCK` into `l_type` when nothing would block, and
leaving the caller's word untouched returns the `F_WRLCK` the caller
conventionally put there, so the answer read "somebody holds this" for ever and
a loop waiting for a lock never left it. All three now report `ENOSYS`. ⭐ The
refusal is temporary in a way the permission one is not: every environment
beneath openkal can lock a byte range, and what is missing is a word in the
specification. Composing one here from `KAL_OPEN_EXCLUSIVE` is not an option —
nothing would release it when its holder died.

**`getppid` returned a negated error value as an identifier.** musl writes it
without `__syscall_ret`, deliberately, because POSIX says it cannot fail; the
default arm answered `-ENOSYS` and a caller was told its parent was -38, with
`errno` untouched. ⚠️ **This is the defect `getpgrp` had and that was fixed one
release earlier, three lines away in the same dispatch, and it was not looked
for.** It answers 0 now — "no parent this environment can name" — and
`examples/surface` asks the whole family rather than the member.

**A copy of the calling image reported its parent's identifier.** `getpid`
answered the constant 1 in every context, so `fork` produced two images that
gave one answer and the copy had no way to name itself. The identifier is now
settled before the copy is taken and carried into it. ⚠️ The comparison `kill`
makes to decide "this program itself" moved with it; against the constant it
would have made `raise`, and therefore `abort`, report `ESRCH` in every copy.

**`setpgid` and `setsid` refused a question neither asks.** They answered
`ENOSYS` on the ground that making a group is not the same as being in one —
but `setpgid(0, 0)` asks to be in a group of one, which `getpgid(0) == getpid()`
already says is true, and `setsid` has a failure POSIX writes down for exactly
that state. They answer 0 and `EPERM`. Every daemonising library handles
`EPERM`; none handles `ENOSYS`.

**`sigaltstack` reported an installation it had not performed**, and the enquiry
answered 0 with a zeroed record rather than "none is installed". `ENOSYS`.

**A bound this library sets was refused rather than stated.** `sysconf(_SC_OPEN_MAX)`
answered 0, because musl reads it from `getrlimit(RLIMIT_NOFILE)` and there was
no case. It answers `OKM_MAX_FD`. Other resources are still refused, because
openkal reports no such limits and inventing one is the shape this port avoids.

**`utimensat` could not set a directory's time.** It asked for
`KAL_OPEN_READ | KAL_OPEN_WRITE` unconditionally and a directory refuses that.
It now asks what the name refers to and opens a directory for reading only. ⚠️
That is outside what `fs.h` states — the interface requires `KAL_OPEN_WRITE`
and has no `kal_dir` form of `kal_fs_set_modified`, so there is no stated route
to a directory's time at all. A file still asks for exactly what is required,
and an implementation that cannot open a directory returns an error that is
passed on unchanged. Asked of the specification.

## What the architecture does not decide alone

`long double`.

musl files `<bits/float.h>` under `arch/<arch>/`, and for every system musl was
written for the architecture does decide it. One of the three systems this port
builds for disagrees: on its aarch64 a long double is a double, where the
architecture's own procedure call standard says quadruple.

Measured on all four combinations this port is built for:

| target | `__LDBL_MANT_DIG__` | `arch/<arch>/bits/float.h` |
| --- | --- | --- |
| aarch64, Apple | **53** | **113** |
| x86_64, Apple | 64 | 64 |
| x86_64, Linux | 64 | 64 |
| aarch64, Linux | 113 | 113 |

Every routine that takes a long double then reads sixteen bytes out of eight.
The first one a program reaches is the one that formats a floating-point number:
`printf("%.3f", 1.5)` stops in `frexpl` with the value read as infinity, and the
report names the arithmetic rather than the header.

`port/include/bits/float.h` states the compiler's own answer on that
combination, through the macros the compiler defines, so the file cannot drift
from what is being compiled. `port/src/okm_float_assert.c` asserts the agreement
for every target: a fourth combination that disagrees is a failed build naming
the field rather than a program that formats a number wrongly.

musl already supports a 53-bit long double --- it is what its own arm and
riscv64 configurations use, and every conditional in `src/math` and `src/stdio`
is written for it. Nothing beyond the one header is involved.
