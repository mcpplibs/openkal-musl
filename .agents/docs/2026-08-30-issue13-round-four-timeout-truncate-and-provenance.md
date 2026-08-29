# openkal-linux#13, round four: what the report contained, and what it did not

2026-08-30 · source examination, measurement, and design

Read against `openkal-musl 250f002` (0.7.0), `openkal-linux 844c2c5` (0.7.0),
`openkal-macos 7b65854` (0.6.0), `openkal d02d3e4` (0.9.0). The consumer's report
names `openkal-llvm-runtime 0.4.0 / openkal-musl 0.7.0 / openkal-linux 0.7.0`.

Statements marked **read** cite a file and a line. Statements marked **measured**
cite the command and its output. Nothing here is marked neither.

---

## 0. Summary

| item in the report | disposition |
| --- | --- |
| the startup crash at `rip = 0`, the only item still blocking | **already fixed, in openkal-musl 0.7.0, and the build measured did not contain it.** §1 |
| `copy_file` reporting `EAGAIN` for two ordinary files | **a defect, in openkal-linux and openkal-macos, not in this port.** Fixed in 0.7.1 and 0.6.1. §2 |
| `truncate` (76) absent, `resize_file` unusable | **a defect here.** Fixed. §3 |
| `last_write_time` on a directory reporting `EISDIR` | **the diagnosis is inverted.** Reading works; setting is refused. §4 |
| the twelve failures on the fork/pipe/dup2/setpgid/poll path | **two causes, one fixed and one open.** §5 |
| `setpgid` (109), `setsid` (112) absent | **open, and it is a question for the specification.** §6 |
| `chmod` (90), `fchmodat` (268), `symlink` (88) | refused by design; `symlink` is answered since 0.7.0. §1.2 |
| `membarrier` (324) | given a case of its own in 0.7.0 so that the trace does not report it. §1.2 |

And one finding the report does not contain, because nothing could have put it
there: **a program built on this port cannot state which version of it it was
built against.** §7.

---

## 1. The startup crash

### 1.1 It was found and fixed before the report was written

openkal-musl 0.7.0 records it. `SYS_rt_sigprocmask` cleared `sizeof(sigset_t)`
into whatever the caller passed:

**read**, `port/src/okm_syscall.c:1697` at `f5c8524` (0.6.0):

```c
sigset_t* old = (sigset_t*)a3;
if (old) for (unsigned i = 0; i < sizeof *old; i++) ((char*)old)[i] = 0;
```

`sizeof(sigset_t)` is 128 (**read**, `musl/include/alltypes.h.in:76`:
`struct __sigset_t { unsigned long __bits[128/sizeof(long)]; }`). The caller
states the size in `a4`, which is `_NSIG/8` = 8.

Sixteen of musl's seventeen callers pass a 128-byte `sigset_t`. The seventeenth
is `musl/src/signal/sigaction.c:65`, `unsigned long set[_NSIG/(8*sizeof(long))]`
— `_NSIG` is 65, so **one word** — and it is reached only when the signal is
SIGABRT. Writing 128 bytes into it destroys the saved frame pointer and the
return address, and `__sigaction` returns to zero.

That accounts for every register in the report, including the one the reporter
correctly identified as anomalous:

| observed | produced by |
| --- | --- |
| `rip = 0` | a `ret` to a cleared return address |
| **stack top all zeroes, not a return address** | the frame is the memory that was cleared |
| `rbp = 0` | the saved frame pointer was in the cleared range |
| every register but `rcx` zero | the same |
| "this isn't a call through a null pointer" | correct, and the correct exclusion pointed the search away from the answer |
| crash inside `InstallSignalHandler` | the terminal library installs a handler for SIGABRT among others, and the defect fires on that number alone |
| a dozen forks and several threads in front of it | not causal. The reproduction is `int main(void){ signal(SIGABRT, h); return 0; }` |

### 1.2 The build that was measured was openkal-musl 0.6.0

Two entries in the reported trace are impossible on 0.7.0, and each is
independent of the other.

**read**, `port/src/okm_syscall.c` at `250f002`:

- `1281`–`1283`: `SYS_symlinkat` and `SYS_symlink` have cases.
- `1825`–`1826`: `SYS_membarrier` has a case of its own, whose stated purpose is
  that the trace not report it.
- `1936`: `trace_absent(n)` is called from the `default:` arm and from nowhere
  else.

Both numbers are defined for this architecture (**read**,
`musl/arch/x86_64/bits/syscall.h.in:89` and `:325`), so both `#ifdef`s are live.
A build carrying 0.7.0 therefore cannot print `88` or `324`, and the report
prints both, at three and seven processes.

Neither can the mixed configuration explain it. Were the C library 0.7.0 and the
backend older, `do_symlink` would refuse from inside its case — after asking
`kal_fs_props` — and the trace still would not name 88; and `membarrier` is
answered in this repository, not beneath it.

Corroborating and weaker: the report gives `rcx = 0xa7e388`, identical to the
value reported for the previous round. 0.7.0 adds several hundred lines to
`okm_syscall.c`, so no address in the image can be expected to survive it.

A consequence worth stating, because it constrains the reply: openkal-llvm-runtime
0.4.0 requires `openkal-musl = "0.7.0"` (**read**, its `mcpp.toml:211`), so a
build holding musl 0.6.0 is holding llvm-runtime 0.3.1. The whole toolchain is
the previous round's.

### 1.3 What follows for the reply

Do not ask whether they upgraded. That question was asked, answered in good
faith, and the answer was wrong — which is a property of the question, not of
the reporter. Give a criterion that passes through no reporting layer:

```
OPENKAL_MUSL_TRACE=enosys <any binary> 2>&1 | grep -E ' (88|324)$'
```

Output means 0.6.0. And ask for `mcpp.lock`, whose own header says it *records
what this build resolved* — it is the artefact that makes a version evidence
rather than a claim.

§7 is the durable half of the same problem.

---

## 2. The `EAGAIN`: a wait upon the wrong descriptor

The two previous rounds attributed this to exhaustion of the started-program
table. The new report disposes of that: it reproduces on a single copy of one
ordinary file, with both paths named.

### 2.1 The chain

1. **read**, libc++ `src/filesystem/operations.cpp:378` — `copy_file` opens the
   *source* with `O_RDONLY | O_NONBLOCK | O_BINARY`. On a kernel this flag is
   meaningless for a regular file and is ignored.
2. **read**, `port/src/okm_syscall.c:146` — `do_read` routes any descriptor
   carrying `O_NONBLOCK` through `okm_timed_read(..., OKM_NOW_NS)`, and
   `SYS_copy_file_range` is a `do_read`/`do_write` loop, so every byte of a file
   copy passes through it.
3. **read**, `openkal-linux/src/timeout.cpp:64-68` (0.7.0) —

   ```cpp
   const int fd  = okl::unpack(s.h);
   const int use = (fd >= 0) ? fd : static_cast<int>(s.h);
   ```

   `okl::unpack` is the decoder for an *owned* handle. A `kal_stream` handle is
   not one: it is the bare descriptor, which that backend's `stream.cpp` states
   in terms and which all four of its producers agree on (`stream.cpp:26-28`,
   `fs.cpp:193`, `net.cpp:136`, `process.cpp:132-133`).
4. **read**, `openkal-linux/src/handle.h` — `pack` is
   `(generation << 32) | (fd + 1)`. A bare descriptor `N` therefore has exactly
   the shape of a packed handle naming `N-1`, and `unpack` accepts it whenever
   the generation recorded for `N-1` is still zero.

**measured**, with that header's arithmetic and nothing else:

```
h= 0  unpack= -1  use= 0  ok
h= 1  unpack=  0  use= 0  *** WRONG DESCRIPTOR ***
h= 2  unpack=  1  use= 1  *** WRONG DESCRIPTOR ***
h= 5  unpack=  4  use= 4  *** WRONG DESCRIPTOR ***
after one packed handle for fd 4 is retired:
h= 5  unpack= -1  use= 5  ok (heals)
```

So the wait was performed upon `N-1` and the transfer upon `N`. `kal_err_again`
from the wait becomes `-EAGAIN` (**read**, `port/src/okm_poll.c:70`), which
`copy_file_range` returns with `done == 0`, and libc++'s fallback list is
`{EINVAL, ENOTSUP, EOPNOTSUPP, ETXTBSY, EXDEV, ENOENT, ENOSYS}` (**read**,
`operations.cpp:310-313`) — `EAGAIN` is not in it, so it neither falls back to
`sendfile` nor to `fstream` and the error reaches the caller verbatim.

### 2.2 Why it behaved as the reporter described

Whether the wrong descriptor is ready is a function of the process's descriptor
history, so the same binary answered differently run alone and run inside a
suite. And the misdecode heals permanently for any index at which an owned
handle has been released, because that advances the generation. Both are exactly
what "does not reproduce standalone" describes, and neither is visible in the
source of the operation that failed.

Empty files are unaffected: libc++ sets `EINVAL` for a zero-length source
(**read**, `operations.cpp:237-240`) and falls back, which is why a copy of an
empty tree passed.

### 2.3 Timeline

| | |
| --- | --- |
| the `O_NONBLOCK` branch in `do_read` | added in openkal-musl **0.5.0** (`ea7fd08`); absent at `dad2e4c` |
| `openkal.timeout` in the backend | added in openkal-linux **0.6.0** (`c096389`) |
| first report of `EAGAIN` | comment 5447166191, on musl 0.5.0 + linux 0.6.0 |

The defect and the symptom arrived in the same release. That A/B was available
for three rounds and was not run.

### 2.4 The fix, and where it is not

Fixed in openkal-linux 0.7.1 and openkal-macos 0.6.1: the two operations taking
a `kal_stream` use the handle as the descriptor. `kal_timeout_accept` and
`kal_timeout_recv_from` keep the decode and are correct — a listener and a
datagram *are* owned. The four call sites divide exactly along the
borrowed/owned line and the two that were wrong were the two holding a borrowed
handle. openkal-windows is unaffected: its `await_stream` takes the `kal_stream`
and never decodes it.

Nothing in this port changes. The route through `O_NONBLOCK` was correct; what
it reached was not.

### 2.5 Why no criterion caught it

Three separate reasons, and each is worth keeping.

- **The specification's own suite cannot fail here.** Its timeout section
  observes `r >= 0 || r == -kal_err_again || r == -kal_err_not_supported` for a
  bounded read of the standard input. Every disjunct is satisfied by a wait upon
  the wrong descriptor. An implementation that expires for the wrong reason is
  indistinguishable there from one that is right.
- **openkal-linux's own suite reached the correct half only.** Its two
  observations are of an `accept`, whose handle is owned and was decoded
  correctly, and of a zero-length write, which returns before it waits.
- **openkal-macos had no observation of the interface at all**, and no workflow
  ran its `tests/` directory.

The criterion added to both makes sixteen channels and reads each with a bound,
and it runs first. Sixteen because under the defect the wait is upon `N-1` and
whether that expires depends on what occupies `N-1`; first because the
misdecode heals for every index at which an owned handle was released, so the
same observation placed later would hold upon the defect.

**measured**, with the old implementation put back:

```
  0 of 16 bounded reads transferred
FAIL: a bounded read of a stream that has bytes waiting transfers them
```

and with the fix, 16 of 16.

---

## 3. `truncate`

**read**: only `SYS_ftruncate` had a case; `SYS_truncate` (76) fell to the
default arm. libc++'s `__resize_file` is `detail::truncate(p.c_str(), size)`
(**read**, `operations.cpp:966`), so `std::filesystem::resize_file` reported
`ENOSYS` for every path.

Composed as `SYS_utimensat` beside it already is: resolve the name, open for
reading and writing, act, release. Opened for both because openkal decides at
the point of opening what may afterwards be done with a file; the divergence
from POSIX that produces is the one already recorded beside `utimensat`.

**measured**, `examples/posix`, with the case and without it:

```
ok: the length is set by name
ok: the length set by name is the length reported
```
```
FAIL: the length is set by name (errno=38)
FAIL: the length set by name is the length reported (errno=38)
```

The descriptor is closed before these run, so neither can reach the operation
that already worked.

---

## 4. `last_write_time` on a directory: the diagnosis is inverted

The report says reading a directory's modification time fails. It does not.

**read**: the reading overload is `posix_stat` → `::stat` → `do_fstatat` →
`kal_fs_info`, which is `newfstatat` and resolves a directory perfectly well
(`openkal-linux/src/fs.cpp:232`). The *writing* overload is `utimensat`, and this
port opens the name as a file (`port/src/okm_syscall.c:1079-1081`) because
`kal_fs_set_modified` takes a `kal_file` and openkal has no form of it that takes
a directory or a name (**read**, `openkal/include/openkal/fs.h:282`). A directory
refuses that open, as `EISDIR`.

**measured**, the two separated:

```
ok: a directory reports its modification time
note: refused with errno=21
ok: setting a directory's modification time is refused
```

The refusal is asserted and the value is only reported, and the reason is a
correction this document owes itself: the first form of that observation
asserted `EISDIR`, and the Windows row of the matrix answered `EACCES`. Both are
right. The port opens the name as a file; what a backend says about opening a
directory as one belongs to the backend, and one of them distinguishes a
directory while another does not. Asserting the first value would have made the
observation a statement about one implementation while reading as a statement
about the port.

**The two overloads produce the same message.** libc++ names both
`"last_write_time"` in the `ErrorHandler` (**read**, `operations.cpp:679` and
`:691`), so `filesystem error: in last_write_time: Is a directory` does not say
which one failed. A lock protocol built on a directory reads the timestamp to
decide staleness and writes it to refresh the lock; only the second fails, and
the reporter has been changing the wrong half.

Recorded in `README.md`'s divergence table, and the refusal is now asserted
rather than merely not exercised, so the row is contradicted if openkal ever
gains the operation.

---

## 5. The fork/pipe/dup2/setpgid/poll path

The report offers an A/B: a suite that drives a shell without pipes, without
`setpgid` and without `poll` passes entirely; a suite that uses all three fails
13 of 17. **That control spans three changes and cannot separate them.**

The pass/fail split is itself the strongest signal in the report:

| passing | failing |
| --- | --- |
| failed exec → 127 | exit 7 → 7 |
| no output → zero chunks | full stdout, in order, in two segments |
| backgrounded descendants die with the group | stdout pipe carries only stdout |
| early EOF leaves no process behind | deadline kill → 124 |
| | the child's `chdir` takes effect |

Every passing observation is satisfied by a child that produced nothing: a
pre-exec failure conventionally exits 127, no output is zero chunks, and a child
that is already gone leaves nothing behind and has no descendants. Every failing
one requires the child to have run. That is the shape of a criterion whose "no"
and whose "did not happen" are the same reading.

Two causes are established and both are real:

1. **§2**, which is on this path: `poll` here is a bounded transfer, and the
   bounded transfer waited on the wrong descriptor. For two pipes made in
   succession, the second pipe's reading end is `N` and `N-1` is the first
   pipe's *writing* end, upon which input is never reported — so the second pipe
   never reports data. That is "the stdout pipe and the stderr pipe are crossed"
   and "the mock server never reports its port".
2. **§6**, `setpgid` reporting `ENOSYS` in the child before it execs.

The report says "the only absent syscall on this path is `setpgid`", and that is
true and incomplete: **the trace reports absences, and §2 is a wrong answer, not
an absence.** The reporter drew exactly this distinction for their own group 2
and did not apply it here.

What would separate the two, one run each: remove `setpgid` and keep the pipes
and the poll; then remove the poll and keep the pipes and `setpgid`.

---

## 6. `setpgid` and `setsid`: open, and it belongs to the specification

**read**: neither has a case, at 0.7.0 or before. The whole of openkal's process
interface is `kal_process_{spawn, spawn_with, wait, terminate, close, channel,
channel_close, props}` — there is no group and no session.

The reporter's use is stated in the report: the child calls `setpgid(0, 0)` so
that a deadline kill takes out the whole descendant tree, and the parent calls
`setpgid(pid, pid)` to close the race. The requirement is not the call; it is
**ending a started program together with everything it started.**

Three answers, and only the third is admissible.

- **Answer `0` and do nothing.** Forbidden by this port's own rule, in terms: no
  operation here reports success having done nothing. It would also be actively
  harmful — a caller that believes it has a group will later kill one, and the
  kill will reach a group that contains the caller.
- **Issue the host's `setpgid`.** This port has no host. Every operation goes
  through openkal, and openkal has no process group.
- **Ask openkal for the property, at the point where it can be given.** A
  process group cannot be composed above the line: `setpgid(0,0)` is issued by
  the child *after* the duplication and before the image is replaced, and this
  port does not control that instant for a `posix_spawn`. What it does control
  is `kal_process_spawn`, and the natural shape is a spawn attribute — start the
  program as the leader of a new group of its own — together with
  `kal_process_terminate` acting upon that group.

That is a specification change, and the argument for it is that the current
`kal_process_terminate` is already incomplete for the case every interactive
program has: a shell that starts a pipeline leaves descendants that
`terminate` cannot reach, whatever the port does.

**Not done in this round.** It is written here so the next round starts from the
question rather than from the symptom, and so that the reply can say the refusal
is a decision rather than an omission.

An interim answer worth measuring first: whether `kal_process_terminate` on this
backend already reaches descendants. If it does not, the reporter's deadline
kill is unreliable in a second way that has nothing to do with `setpgid`.

---

## 7. A build cannot state which version of this port it holds

Three rounds of this issue have turned on which version was in a binary, and the
consumer has no way to answer that except by reporting what they believe they
installed.

**read**: the only thing in this port resembling a version is
`SYS_uname`'s `release` field, which is the string literal `"0.5.0"` at 0.7.0
(`port/src/okm_syscall.c:1848`). It is not merely stale; it is a hard-coded
constant that no release has ever moved. There is no other symbol a program can
read.

So `uname -r` is a *lying* oracle, which is worse than none: a consumer who
checks it is told a version, and the version is wrong.

Two changes, neither large:

- Derive the `release` field from the package version at build time, so that
  `uname -r` answers.
- Have `OPENKAL_MUSL_TRACE` print one line naming the version before anything
  else, so that a trace pasted into an issue carries its own provenance.

The second matters more, because the trace is what a reporter is already asked
to paste. A report that carries its version is a report that cannot be answered
against the wrong one, which is the whole of what went wrong this round.

---

## 8. Criteria

| criterion | where | fails on the defect? |
| --- | --- | --- |
| a bounded read of a stream with bytes waiting transfers them, sixteen times | openkal-linux `tests/conformance_v08.cpp`, openkal-macos `tests/conformance_timeout.cpp` | **measured**: `0 of 16` |
| a bounded read of a stream with nothing waiting expires | the same | no — it is a control, and holds under both readings |
| the length is set by name | `examples/posix` | **measured**: `errno=38` |
| a directory reports its modification time | `examples/posix` | it is the half that works, and is recorded so that the other half is not read as this one |
| setting a directory's modification time is refused as `EISDIR` | `examples/posix` | it asserts a documented refusal; it fails if openkal gains the operation, which is when the README row must change |
| `mcpp test` runs, and every suite in `tests/` reported `ok` | openkal-macos CI | **measured**: it had never run, and the first run failed |

Two meta-rules, both earned this round.

**A criterion placed after the state it depends on is not a criterion.** The
timeout observation holds upon the defect if it runs after anything that
releases an owned handle, because the misdecode heals per descriptor index. It
runs first, and the reason is written beside it.

**A suite that is never invoked is not evidence, and its own text will say so.**
openkal-macos's five suites all announced themselves as `openkal-linux: ...` and
one of them did not compile against openkal 0.9. Both were invisible for as long
as nothing ran them.

---

## 9. What was deliberately not done

- **`setpgid`/`setsid` are not implemented.** §6.
- **The `EISDIR` from `utimensat` is not changed to `ENOSYS`.** It is arguably
  the more honest value — the port has no operation, rather than the caller
  having asked something forbidden — but the reporter is acting on the current
  value and the row now says what it means. Changing an error code a consumer
  handles, in the same release that fixes what they reported, would make the two
  hard to tell apart.
- **The dead retry in openkal-linux's `tests/conformance_process_task.cpp` is
  left.** The same test in openkal-macos chose a program to start by spawning
  and retrying, which cannot work: `kal_process_spawn` reports whether the
  duplicate was made and the image is replaced afterwards. It was corrected
  there because it failed there. openkal-linux holds only because its first
  candidate exists, and it should be corrected in a change of its own rather
  than folded into an unrelated fix.
- **No new `openkal.fs` operation is requested** for a directory's modification
  time. The case for one is weaker than for process groups: a caller can hold a
  file inside the lock directory instead.
