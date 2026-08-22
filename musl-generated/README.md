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

## The Windows variant

`x86_64-windows/` differs from `x86_64/` in three lines, and the three lines are
the whole of what makes musl a library for one data model:

```
-#define _Addr long          +#define _Addr long long
-#define _Reg long           +#define _Reg long long
-#define _Int64 long         +#define _Int64 long long
```

musl is written for LP64 — a `long` holds a pointer — and every one of its
architectures is LP64 or ILP32. Windows is LLP64: a `long` is thirty-two bits
and a pointer is sixty-four. The three definitions above are where musl states
the model, so stating a different one is where the difference begins.

It is not where it ends. The rest is recorded in the package README under
"What Windows required", because a reader who finds these three lines is
entitled to know that they were not sufficient.

## riscv64

Added 2026-08-22 by the same three commands, with `ARCH=riscv64`. The
architecture directory it needs (`musl/arch/riscv64/`) was taken from the same
musl 1.2.5 release the rest of this tree is, and its absence — not any property
of the C++ runtime above — was what stopped this package building for a machine
with no operating system.
