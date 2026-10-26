/* An image that another places with `dlopen'. It holds no C library: its
 * imports are `kal_*' alone, and what it exports is marked. */
typedef unsigned long kal_uintptr;
typedef long kal_intptr;
struct kal_stream { kal_uintptr h; };
struct kal_stream kal_stdout(void);
kal_intptr kal_stream_write(struct kal_stream, const void*, kal_uintptr);

#define EXPORT __attribute__((visibility("default")))

EXPORT int plug_ready;
EXPORT void (*plug_on_close)(void);

__attribute__((constructor)) static void up(void) { plug_ready = 1; }
__attribute__((destructor)) static void down(void) { if (plug_on_close) plug_on_close(); }

EXPORT int plug_twice(int x) { return 2 * x; }
EXPORT int plug_call(int (*callback)(int), int v) { return callback(v) + 1; }
EXPORT long plug_say(void)
{
	static const char line[] = "plugin: written through kal_stream_write\n";
	return kal_stream_write(kal_stdout(), line, sizeof line - 1);
}
