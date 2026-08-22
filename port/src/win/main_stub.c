/* `__main`, which this format's compiler emits a call to and this C library has
 * already made unnecessary.
 *
 * ⭐ WHAT IT IS. On a PE target the compiler emits `call __main` as the first
 * thing `main` does. In the arrangement that name comes from, that call runs the
 * program's global constructors: the C runtime's startup does not, and the
 * responsibility sits at the top of `main` instead.
 *
 * This C library's startup does. `__libc_start_main` walks `.init_array` before
 * it calls `main`, on every format, which is the arrangement every other target
 * of this port already uses. So the constructors are run exactly once, before
 * `main`, and `__main` has nothing left to do.
 *
 * ⚠️ IT CANNOT SIMPLY BE ABSENT. The call is in the object whether or not
 * anything defines the name, so leaving it undefined is a link error rather
 * than a saving:
 *
 *     ld.lld: error: undefined symbol: __main
 *
 * measured 2026-08-22 after `-lgcc` came off this format's link line — that
 * archive had been supplying it, together with `___chkstk_ms`, and neither is a
 * routine this port should be taking from a different compiler's runtime.
 *
 * ⚠️ AND IT MUST NOT RUN THE CONSTRUCTORS ITSELF. Defining it as a second walk
 * of `.init_array` would run every one of them twice, which is not a failure
 * the linker can report.
 */
void __main(void);
void __main(void) {}
