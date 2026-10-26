/* The C library up before any other initialiser of the image.
 *
 * The loader runs an image's initialisers before its entry (SPEC 12.3), so a
 * program's own constructor can allocate, format and read the environment
 * before `ok_main' is reached. The Mach-O form of this file runs from an
 * unprioritised constructor because that format orders initialisers by image;
 * here they are one array, ordered by the link, and a program's object comes
 * before the library's --- measured: the example's constructor called `malloc'
 * with the allocator's secret not yet drawn, and faulted. A priority puts this
 * one first: the linker sorts `.init_array.<priority>' ahead of the rest. */
void __okm_libc_init(void);

__attribute__((constructor(101))) static void okm_image_init(void)
{
	__okm_libc_init();
}
