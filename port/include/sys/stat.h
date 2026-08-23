/* musl's <sys/stat.h>, plus the field names this object format's consumers
 * expect.
 *
 * ⭐ THE SAME QUESTION EVERY OTHER OVERLAY IN THIS ECOSYSTEM ANSWERS: consumers
 * ask "which OPERATING SYSTEM is this" and assume that system's C library.
 * Here the C library is musl, whatever the object format.
 *
 * `struct stat`'s sub-second timestamps have two spellings. POSIX standardised
 * `st_mtim` / `st_atim` and musl provides those. Apple's platforms predate the
 * standardisation and use `st_mtimespec` / `st_atimespec`, and consumers that
 * key on `__APPLE__` reach for the second set. Measured 2026-08-23,
 * cross-compiling libc++ for arm64-apple-macos over this library:
 *
 *     time_utils.h:277: no member named 'st_mtimespec' in 'stat'
 *     time_utils.h:278: no member named 'st_atimespec' in 'stat'
 *
 * ⚠️ ALIASES, NOT A SECOND SET OF FIELDS. There is one `struct stat` and one
 * layout — musl's — and this only gives two of its members a second name. A
 * consumer that writes either spelling reaches the same bytes, which is what
 * makes this safe: nothing here changes what crosses an interface.
 *
 * ⚠️ SCOPED TO THE FORMAT THAT ASKS. On ELF and PE nothing spells them this
 * way, and defining the names there would put two identifiers into every
 * translation unit for no reason.
 *
 * ⇒ The alternative was to turn `<filesystem>` off for this target, which is a
 * `__config_site` switch and would have worked — and would have removed a
 * working facility to route around a spelling.
 */
#ifndef OKM_SYS_STAT_H
#define OKM_SYS_STAT_H

#include_next <sys/stat.h>

#if defined(__APPLE__)
#  define st_mtimespec st_mtim
#  define st_atimespec st_atim
#  define st_ctimespec st_ctim
#endif

#endif /* OKM_SYS_STAT_H */
