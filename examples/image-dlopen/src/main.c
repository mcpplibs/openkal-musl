/* `dlopen' in an ok image: eleven observations of libokld placing another
 * image beneath openkal-musl. The image to place is the first argument. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <stdio.h>
#include <string.h>

static int failures;
static void check(int held, const char* what)
{
	printf("%s %s\n", held ? "  ok  " : "  FAIL", what);
	if (!held) failures++;
}

static int doubled_by_host(int v) { return v * 10; }
static int closed;
static void on_close(void) { closed = 1; }

struct seen { int objects; int plugin; };
static int count(struct dl_phdr_info* info, size_t size, void* data)
{
	(void)size;
	struct seen* s = data;
	s->objects++;
	if (info->dlpi_name && strstr(info->dlpi_name, "libplug")) s->plugin = 1;
	return 0;
}

int main(int argc, char** argv)
{
	if (argc < 2) { puts("usage: image-dlopen <libplug.okso>"); return 2; }

	check(dlopen("no-such-image.okso", RTLD_NOW) == 0, "an image that is not there is not placed");
	const char* why = dlerror();
	check(why && strstr(why, "no-such-image"), "dlerror names it");

	void* h = dlopen(argv[1], RTLD_NOW);
	if (!h) printf("  (dlerror: %s)\n", dlerror());
	check(h != 0, "the image is placed");
	if (!h) { printf("-- failures: %d --\n", failures); return 1; }

	int (*twice)(int) = (int (*)(int))dlsym(h, "plug_twice");
	check(twice != 0, "dlsym finds an export");
	check(twice && twice(21) == 42, "the export is called and returns");
	int* ready = dlsym(h, "plug_ready");
	check(ready && *ready == 1, "the image's constructor ran when it was placed");
	check(dlsym(h, "plug_absent") == 0 && dlerror() != 0, "a name it does not export is reported");

	Dl_info info;
	check(dladdr((void*)twice, &info) && info.dli_fname && strstr(info.dli_fname, "libplug")
	      && info.dli_fbase != 0, "dladdr names the image an address is in");

	struct seen s = { 0, 0 };
	dl_iterate_phdr(count, &s);
	check(s.objects >= 2 && s.plugin, "dl_iterate_phdr reports this image and the placed one");

	int (*call)(int (*)(int), int) = (int (*)(int (*)(int), int))dlsym(h, "plug_call");
	check(call && call(doubled_by_host, 4) == 41, "the placed image calls back into this one");

	long (*say)(void) = (long (*)(void))dlsym(h, "plug_say");
	check(say && say() > 0, "the placed image's kal_* import is bound");

	void (**hook)(void) = dlsym(h, "plug_on_close");
	if (hook) *hook = on_close;
	check(dlclose(h) == 0 && closed, "dlclose runs its destructor");

	printf("-- failures: %d --\n", failures);
	return failures ? 1 : 0;
}
