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

Nine, and the list in the manifest carries the same reasons. Five read the shape
of one environment directly. Two carry a machine word through a variable
declared `long`. Two more were found only by running the result:

`src/mman/mmap.c` returns a pointer through a `long`. It is replaced rather than
patched because the replacement is also better where a `long` does hold a
pointer: the value never becomes an integer at all.

`src/unistd/getcwd.c` refuses an answer that does not begin with a separator.
That is correct on every system musl was written for and is not correct on one
that writes a volume first, and openkal does not say which a system does.

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
the status it ended with. A caller cannot distinguish that through this library
--- the same program runs, with the same arguments, on the same streams, and the
same status reaches whoever waits --- but there are two images where a system
with the operation would have one, so the identifier the started program reports
is not the caller's. It is the arrangement every environment without the
operation uses, and two of the three beneath openkal are such environments.

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

`fork` is absent and stays absent. Duplicating a running image is not something
every environment can produce. `posix_spawn`, and therefore `system` and
`popen`, are supplied, because musl builds them on starting a program.

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
