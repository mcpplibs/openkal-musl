# Generated headers

musl generates three headers rather than shipping them, and its build system
does so with `sed` at configure time. This package has no configure step, so
they are generated once and committed, and the commands that produce them are
recorded here so that a reader can reproduce them from the vendored sources.

```sh
sed -f musl/tools/mkalltypes.sed \
    musl/arch/$ARCH/bits/alltypes.h.in musl/include/alltypes.h.in \
    > musl-generated/$ARCH/bits/alltypes.h
cp musl/arch/$ARCH/bits/syscall.h.in musl-generated/$ARCH/bits/syscall.h
sed -n -e 's/__NR_/SYS_/p' < musl/arch/$ARCH/bits/syscall.h.in \
    >> musl-generated/$ARCH/bits/syscall.h
printf '#define VERSION "1.2.5"\n' > musl-generated/internal/version.h
```

## The Windows variant, and why it is no longer a variant

`x86_64-windows/` used to differ from `x86_64/` in three lines:

```
-#define _Addr long          +#define _Addr long long
-#define _Reg long           +#define _Reg long long
-#define _Int64 long         +#define _Int64 long long
```

musl is written for LP64 — a `long` holds a pointer — and every one of its
architectures is LP64 or ILP32. Windows machine code is unchanged by any of
this: it is still PE and Win64. What used to differ was the *environment*
this package presented to source text compiled for it — LLP64, the data
model the platform's own C runtime uses — stated in the three lines above.

**Since `openkal-musl` 0.15.0 this package states LP64 for Windows as well**,
through the `[c-abi]` block in the package manifest (`data-model =
"arch-default"`, which is musl's own answer for the architecture — LP64 on
x86_64 everywhere musl runs it, Windows now included). Regenerating this
header for that statement, from the same `musl/arch/x86_64/bits/alltypes.h.in`
the native x86_64 header is generated from, produces the byte-identical file:
there is no longer a Windows-specific line to record. The file is kept as its
own directory rather than replaced by a reference to `x86_64/` because it
names its own target, and because it is the seam a future divergence — a
second Windows architecture, or a reason to diverge again — would land in.

Reproduced by the same three commands as every other architecture, with
`ARCH=x86_64`, once the package presents LP64 for the target: the command
does not read `windows` at all, only the architecture directory.

The wide-character literal this change also settles — `wchar_t` at 32 bits
rather than 16 — is not stated in this file. musl's own `alltypes.h.in`
already typedefs `wchar_t` as `int` for every architecture it ships
(visible above, under `__NEED_wchar_t`); what disagreed with it was the
*compiler's* built-in `wchar_t` for `L"..."` literals, which is outside a
generated header and is stated by the C library's `[c-abi]` block
(`wchar = 32`) instead. See the package README and `musl/PATCHES.md` for what
that used to cost.

## riscv64

Added 2026-08-22 by the same three commands, with `ARCH=riscv64`. The
architecture directory it needs (`musl/arch/riscv64/`) was taken from the same
musl 1.2.5 release the rest of this tree is, and its absence — not any property
of the C++ runtime above — was what stopped this package building for a machine
with no operating system.

## The macOS variants

`aarch64-macos/` and `x86_64-macos/` each differ from the generic architecture
directory beside them, and neither difference is the whole three lines this
file's own generation commands produce — each is a small, direct override of
what those three lines alone would compute, because Apple's platforms disagree
with Linux's on more than the `_Addr`/`_Reg`/`_Int64` scheme alone expresses.

Apple's platforms are LP64, exactly as musl's architectures are — a `long`
holds a pointer — so `_Addr` and `_Reg` are unchanged on both. What differs:

| | `aarch64-macos` vs `aarch64` | `x86_64-macos` vs `x86_64` |
| --- | --- | --- |
| `_Int64` | `long` → `long long` | `long` → `long long` |
| `wchar_t` | `unsigned` → `int` | unchanged (`int` already) |
| `wint_t` | `unsigned` → `int` | `unsigned` → `int` |
| `intmax_t` / `uintmax_t` | no longer derived from `_Int64`; stated directly as `long` / `unsigned long` | same |

`_Int64` (`int64_t`, `uint64_t`, `off_t`, `time_t` and their neighbours) was
the first found, 2026-08-23, cross-compiling this port for `arm64-apple-macos`
from Linux:

```
okm_syscall.c:439: incompatible pointer types passing 'uint64_t *'
  (aka 'unsigned long *') to parameter of type 'kal_u64 *'
  (aka 'unsigned long long *')
```

`kal_u64` is derived from `__UINT64_TYPE__`, which is the compiler's statement
about the target's ABI and is authoritative; `uint64_t` came from these three
lines, which carried Linux's answer to a question that has a different answer
here.

The other three were not caught by a build failure — `port/src/okm_syscall.c`
is the only place in this port that took a `uint64_t*` where a `kal_u64*` was
wanted, and nothing else in this port passes a `wchar_t`, `wint_t`,
`intmax_t` or `uintmax_t` across a boundary strict enough to refuse an
incompatible type outright. They were found by
`port/src/okm_type_identity.c`, which checks every one of these generated
typedefs against the compiler's own builtin macro for the same type
(`__WCHAR_TYPE__`, `__WINT_TYPE__`, `__INTMAX_TYPE__`, `__UINTMAX_TYPE__` and
the rest), for the actual target being built, at compile time, on every
target this package builds for — rather than waiting for a probe that happens
to exercise the one that disagrees. `wchar_t` was the case a probe HAD
exercised (`examples/c-abi`'s wide string literal, on `aarch64-macos`); the
same check, run for `x86_64-apple-macos` — which nothing in this repository's
CI builds for, but which the generic `not(windows)` row silently claimed to
answer for before this — found the other three without any probe naming them
at all. `intmax_t`/`uintmax_t` could not be fixed the way `int64_t` was,
by changing what `_Int64` expands to, because Apple's own `intmax_t` is
`long` while its `int64_t` is `long long` — two different 64-bit types on
the same target — so the generated header states them directly instead of
deriving them.

`x86_64-macos/` did not exist before the check above found it was needed:
until then, `x86_64` targeting macOS fell into the generic
`cfg(all(arch = "x86_64", not(windows)))` row and silently got Linux's
answer for all four of these, untested, because nothing in this repository
builds or runs for it.
