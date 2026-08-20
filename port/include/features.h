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

#if defined(_WIN32)

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
 * real definition makes it instead. */
#define OKM_ALIAS_0(old, new) extern __typeof(old) new __attribute__((__alias__(#old)))
#define OKM_ALIAS_1(old, new) extern __typeof(old) new
#define OKM_ALIAS_PICK2(x)    OKM_ALIAS_##x
#define OKM_ALIAS_PICK(x)     OKM_ALIAS_PICK2(x)

#undef weak_alias
#define weak_alias(old, new) OKM_ALIAS_PICK(OKM_IS_PLACEHOLDER(old))(old, new)

#endif  /* _WIN32 */

#endif
