/* Bringing this library up before the C++ runtime's static initialisers.
 *
 * ⭐ WHY THIS FILE EXISTS ONLY FOR THIS OBJECT FORMAT. The dynamic loader here
 * runs an image's constructors BEFORE it transfers control to the entry point,
 * which is the opposite of the order every other format this port serves uses.
 * `port/src/okm_start.c` records the measurement: libc++ constructs its
 * standard streams over this library's `stdin`, from a constructor, and on this
 * format that happens while `okm_start` has not run.
 *
 * ⇒ A constructor of our own, so that the library is up before any other
 * constructor can reach it.
 *
 * ⚠️ ORDER AMONG CONSTRUCTORS IS LINK ORDER, AND THAT IS WHAT MAKES THIS WORK
 * RATHER THAN THE PRIORITY. A priority argument is honoured within one
 * translation unit and not across archives on this format. What does hold is
 * that this package is a DEPENDENCY of the C++ runtime, so its objects precede
 * that runtime's on the link line, and the loader runs them in that order.
 * Measured: the ten module initialisers of `openkal` ran first, then this
 * library's, then libc++'s.
 *
 * ⚠️ AND THE GUARD IS WHAT MAKES IT SAFE RATHER THAN THE ORDER. `okm_start`
 * still calls the same function; whichever arrives first does the work and the
 * second returns immediately. A fix that depended on the order being right
 * would be a fix that fails silently when it is not.
 */
void __okm_libc_init(void);

__attribute__((constructor)) static void okm_early_init(void)
{
	__okm_libc_init();
}
