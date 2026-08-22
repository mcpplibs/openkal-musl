/* How a second name for a definition is made, where the object format has no
 * weak one.
 *
 * musl gives almost every public name to a definition that is named something
 * else: `fstat' is a weak alias of `__fstat', `malloc' of the allocator's own
 * entry. There are 289 such aliases. Weakness does two different jobs among
 * them, and the difference is what this header is about.
 *
 *   It lets a program supply its own `malloc' and have the library's yield.
 *
 *   It lets one of musl's own sources supply a placeholder that another source
 *   replaces --- 46 names are provided that way, and every one of the
 *   placeholders is an alias of a local, empty definition whose name begins
 *   with "dummy".
 *
 * The measurement, made with both toolchains this package is built by:
 *
 *     int base(void) { return 7; }
 *     extern __typeof(base) w __attribute__((__weak__, __alias__("base")));
 *     extern __typeof(base) s __attribute__((__alias__("base")));
 *
 * On an ELF target both `w' and `s' are defined. On this object format `s' is
 * defined and `w' is not: it becomes a record named `.weak.w.base' that neither
 * GNU ld nor lld ever resolves, so a program referring to `w' fails to link
 * naming it, and a weak *reference* to it evaluates to a null pointer. The same
 * holds for a weak definition that is not an alias, and for data as well as
 * functions. This object format, as both linkers implement it, has no weak
 * symbol that is a definition.
 *
 * So a second name is made strongly, and the placeholders --- which would then
 * collide with the sources that replace them --- are not made at all. Which is
 * which is decided by the name of the target, because musl's placeholders are
 * recognisable by it and by nothing else.
 *
 * A third object format reaches the same conclusion by a different route. There
 * the compiler refuses the construct outright --- "aliases are not supported on
 * darwin" --- so neither form above can be written. What that format does have
 * is the assembler's own way of giving an address a second name, and the same
 * measurement establishes it:
 *
 *     int base(void) { return 7; }
 *     __asm__(".globl _w\n\t.set _w, _base");
 *
 * produces `w' as a definition at `base's address. That is a strong name, so the
 * placeholder list below applies there too, and the underscore is written out
 * because that format prefixes every C name with one.
 *
 * Two consequences follow, and both are boundaries rather than defects.
 *
 *   A program cannot replace a function of this library by defining one of the
 *   same name. It is told so by the linker, which is the loudest report
 *   available and is preferable to the alternative: the library's definition
 *   winning silently while the program believes its own is in use.
 *
 *   A placeholder that nothing replaces is now absent rather than empty. Those
 *   are supplied by port/src/okm_pe_defaults.c, which lists them, and the list
 *   was obtained by removing the placeholders and reading what the linker then
 *   reported.
 */
#ifndef OKM_FEATURES_H
#define OKM_FEATURES_H

#include_next <features.h>

/* ⚠️ THE INTERNAL HEADERS ARE C, AND A CONSUMER MAY NOT BE.
 *
 * musl's build reaches its own declarations through src/include, whose headers
 * add the hidden entries the public ones do not have. This package publishes
 * the path it is built from --- one set of directories rather than two, which
 * is the decision mcpp.toml records --- so a consumer reaches src/include too.
 *
 * ⓘ THIS IS THE SECOND-BEST REMEDY. The first would be for a package to
 * distinguish the directories it is built from from the directories it
 * publishes. Measured 2026-08-22: mcpp cannot express it. Moving the two
 * directories into per-glob flags places them AFTER include_dirs on the command
 * line, and musl's own build then finds the public <features.h> before the
 * internal one and fails with `unknown type name hidden'. The note is here so
 * that the better fix is not lost. */
#ifdef __cplusplus
/* The rule, rather than a remedy per collision.
 *
 * musl's own sources are C and are never compiled as anything else. So a
 * translation unit that is C++ is, with certainty, NOT one of them --- it is a
 * consumer that reached these headers because this package publishes the path
 * it is built from. Everything src/include adds for musl's own build is
 * therefore inert here, and what it must not do is collide with the consumer.
 *
 * Three names, and they were found one at a time by two consumers rather than
 * by reading:
 *
 *   restrict     a C keyword, not a C++ one. `f(int *restrict, int *restrict)'
 *                is read as two parameters both named `restrict'. Given the
 *                spelling every compiler accepts in both languages.
 *
 *   hidden, weak attributes musl spells as bare words. Made empty rather than
 *                removed, because src/include's declarations use them and a
 *                consumer that includes <pthread.h> reaches those declarations.
 *
 *   weak_alias   REMOVED rather than emptied. It is used in musl's .c files and
 *                in no header, so nothing here needs it --- and leaving it
 *                defined breaks any consumer that writes
 *                `__attribute__((weak_alias(...)))' of its own. LLVM's
 *                libunwind does, in fifteen places, and reported
 *                `use of undeclared identifier __weak__'.
 *
 * ⓘ Still the second-best remedy, for the reason recorded below: a package that
 * could distinguish the directories it is built from from the directories it
 * publishes would need none of this. */
#  if !defined(restrict)
#    define restrict __restrict
#  endif
#  undef hidden
#  define hidden
#  undef weak
#  define weak
#  undef weak_alias
#endif

/* Not for a C++ consumer: it redefines weak_alias, and the block above removed
 * that name deliberately. musl's own sources are C, so this never applies to them. */
#if (defined(_WIN32) || defined(__APPLE__)) && !defined(__cplusplus)

/* Whether a name is one the preprocessor has been told about. The idiom is the
 * usual one: a name that has been told about expands to a marker that shifts
 * the argument list, and one that has not expands to itself and does not. */
#define OKM_SECOND(a, b, ...) b
#define OKM_CHECK(...)        OKM_SECOND(__VA_ARGS__, 0, )
#define OKM_MARK(x)           x, 1,
#define OKM_IS_PLACEHOLDER(target) OKM_CHECK(OKM_PLACEHOLDER_##target)

/* Every name musl gives a placeholder alias to. The list is the set of local,
 * empty definitions musl aliases from, and it is closed: `grep' over the
 * vendored sources produces exactly these seven. */
#define OKM_PLACEHOLDER_dummy              OKM_MARK(~)
#define OKM_PLACEHOLDER_dummy_0            OKM_MARK(~)
#define OKM_PLACEHOLDER_dummy1             OKM_MARK(~)
#define OKM_PLACEHOLDER_dummy_file         OKM_MARK(~)
#define OKM_PLACEHOLDER_dummy_gettextdomain OKM_MARK(~)
#define OKM_PLACEHOLDER_dummy_lockptr      OKM_MARK(~)
#define OKM_PLACEHOLDER_dummy_tsd          OKM_MARK(~)

/* Three placeholders musl gives an ordinary name rather than a dummy one. Each
 * is a simple implementation that a fuller one replaces: the bump allocator
 * that the real allocator replaces, the plain system call that the cancellable
 * one replaces, and the conservative answer that the allocator's own replaces. */
#define OKM_PLACEHOLDER___simple_malloc    OKM_MARK(~)
#define OKM_PLACEHOLDER_sccp               OKM_MARK(~)
#define OKM_PLACEHOLDER_allzerop           OKM_MARK(~)

/* 0: the second name is made. 1: it is not, and the source that supplies the
 * real definition makes it instead.
 *
 * The two formats differ only in how a second name is written. One accepts the
 * compiler's construct without its weakness; the other refuses the construct
 * and accepts the assembler's directive, where the name carries the leading
 * underscore that format gives every C name. */
#if defined(__APPLE__)
/* The directive names the definition, and the pointer keeps it.
 *
 * A directive in module-level assembly is opaque to the compiler, so a
 * definition that nothing else in the translation unit refers to is deleted
 * before the assembler ever sees the name --- and musl has six such: a static
 * function that exists only to be given a second name. What the compiler then
 * emits is a second name for nothing, which the linker reports as an undefined
 * symbol with the alias as its only reference.
 *
 * The pointer is a reference the compiler does understand. It is `used' so that
 * it is emitted rather than folded away, and taking the address of the
 * definition is what obliges the compiler to keep it.
 *
 * The other object format needs none of this: there the second name is made
 * with the compiler's own construct, which is itself a use. */
#  define OKM_ALIAS_0(old, new)                                          \
        __asm__(".globl _" #new "\n\t.set _" #new ", _" #old);           \
        static __typeof(old)* const OKM_KEEP_##new                       \
            __attribute__((__used__)) = &old
#else
#  define OKM_ALIAS_0(old, new) extern __typeof(old) new __attribute__((__alias__(#old)))
#endif
#define OKM_ALIAS_1(old, new) extern __typeof(old) new
#define OKM_ALIAS_PICK2(x)    OKM_ALIAS_##x
#define OKM_ALIAS_PICK(x)     OKM_ALIAS_PICK2(x)

#undef weak_alias
#define weak_alias(old, new) OKM_ALIAS_PICK(OKM_IS_PLACEHOLDER(old))(old, new)

#endif  /* _WIN32 || __APPLE__ */

#endif
