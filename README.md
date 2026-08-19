# openkal-libc

A C library above [openkal](https://github.com/mcpplibs/openkal) rather than
above a kernel.

The package exists to test a claim. The claim is that porting one library causes
the software above it to run on every implementation of openkal, and the claim
is only testable if such a library exists.

```toml
[dependencies]
openkal      = "0.3.0"
openkal-libc = "0.1.0"

[target.'cfg(os = "linux")'.dependencies]
openkal-linux = "0.3.0"
[target.'cfg(os = "macos")'.dependencies]
openkal-macos = "0.1.0"
```

## The two pieces of work this library performs

Both are placed here by the specification, and both are placed here for the same
reason: they are adaptations a program expects, and an interface that performed
them would have assumed the environment could.

**Resolving a global name.** openkal has no global namespace: every operation is
relative to a directory the environment supplied. A program written for a hosted
system does not know that and does not have to. This library selects the
supplied directory whose name is the longest prefix of the path and opens the
remainder relative to it — one rule, in one place, rather than the same rule in
every program.

A consequence worth stating: a program confined to a subtree finds no supplied
directory for a name outside it, and therefore fails to resolve the name rather
than reaching outside. Confinement is a property of what the environment
supplied, not of the program's cooperation.

**Constructing synchronisation objects.** openkal declares a primitive that
suspends a context upon a word until another wakes it. A mutex is what a program
asks for; the primitive is what an environment can supply. The construction is
here, which is the relation a C library has to a kernel on any system that
provides such a primitive.

## What it provides

| | |
| --- | --- |
| paths | resolution of a global name against the supplied directories |
| files | opening, reading whole files, releasing |
| output | counted and scanned writing, and unsigned decimal |
| synchronisation | a mutex built from the suspension primitive |

The surface is the part the tests exercise. It is not a complete C library, and
the completeness plan records what a complete one requires.

## Verification

`mcpp test` runs three suites. The mutex suite is contended deliberately: an
uncontended mutex never enters the environment, so a suite exercising only that
path would pass with the primitive not working at all.

`examples/wordcount` is an ordinary program. It reads a file named on its
command line using a global path, consults an environment variable, measures an
interval and starts another program, and its source contains none of the
adaptation that makes those possible. Its output is compared against the
system's own tool, which is an oracle the implementation did not produce.

## License

Apache-2.0.
