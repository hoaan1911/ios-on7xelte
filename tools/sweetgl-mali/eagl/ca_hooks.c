// SPDX-License-Identifier: MIT
// Galaxy S7 port: the part of HakiOS's src/inject.c (oosexgang/hackios-xnu-kernel, MIT) that eagl.c needs
// from inside CoreAnimation, for our dyld shared cache (iOS 16.7, iPhone10,3) instead of their 16.5 (20F66).
//
// CA::OGL::GLESContext::function_uniform() hands a draw's "function" its numbers (a colour matrix, the
// places of a blur); for the functions the OpenGL ES renderer has no shader for, the draw is made with
// no program and the numbers go nowhere. eagl.c draws those with programs of its own (plain_draw_begin,
// group_draw_begin) and needs the numbers and the context, from which it reads which function a draw
// is. Without them every such draw was a plain picture: the lock screen's vibrancy gradient came out as
// a red veil over the wallpaper. function_supported() is watched to say which functions CoreAnimation
// asks for and is told are missing (what needs them is not drawn), and to count them in eagl.c.
//
// The addresses were found with tools/dsc-find.py: the shader table by its sources ("ds4", 26, has
// the weights of four samples; 20, 28, 40 and 45 are null as in 16.5), the code that reads the
// function number at context+0x10 -> +0x10 and compares it with 89, and the one vtable that holds it.
// Both entries are checked before they are replaced; on any other cache nothing is touched.
#include <mach/mach.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

extern const void *_dyld_get_shared_cache_range(size_t *length);
void hakios_gl_function_values(void *context, unsigned function, unsigned index, size_t components, size_t count,
    const float *values);
void hakios_gl_function_refused(unsigned function);

#define CACHE_BASE              0x180000000UL
#define VT_GLES_CONTEXT         0x1d5b2f6e8UL   /* vtable for CA::OGL::GLESContext (16.5: 0x1d5b3b540) */
#define VSLOT(vtable, offset)   ((vtable) + 0x10 + (offset))
#define FUNCTION_SUPPORTED      0x1884b2164UL   /* slot 0x378 (16.5: 0x1884a9308) */
#define FUNCTION_UNIFORM        0x1884b21acUL   /* slot 0x388 (16.5: 0x1884a9350) */

static uintptr_t slide;

static void
say(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void
say(const char *format, ...)
{
	char line[256];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	write(2, line, (size_t)n);
}

static void *
replace_virtual(const char *what, uintptr_t slot, uintptr_t expected, void *replacement)
{
	uintptr_t *entry = (uintptr_t *)(slot + slide);
	vm_address_t page = (vm_address_t)entry & ~(vm_address_t)(vm_page_size - 1);
	kern_return_t kr;
	void *original;

	if (*entry != expected + slide) {
		say("hakios-ca: %s: found %#lx, expected %#lx; left alone\n", what, *entry - slide, expected);
		return NULL;
	}
	/* the tables are in read-only memory shared by every process: a private writable copy of the page */
	kr = vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
	if (kr != KERN_SUCCESS) {
		say("hakios-ca: %s: cannot make the table writable (%d)\n", what, kr);
		return NULL;
	}
	original = (void *)*entry;
	*entry = (uintptr_t)replacement;
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
	return original;
}

/*
 * Boot-arg glsb3=1: CoreAnimation's function 40 ("sb3", the four samples of a small shadow blur), for which
 * the renderer has no shader, is given one in its table of sources (HakiOS: inject.c, shadow_blur_source;
 * ds4's shader without the weights) and function_supported() says yes for it. Without it, what needs the
 * function is not drawn.
 */
#define CA_SHADER_TABLE         0x1d5b48a10UL   /* indexed by function (16.5: 0x1d5b54790) */
#define FUNCTION_SHADOW_BLUR    40

static const char shadow_blur_source[] =
    "uniform lowp sampler2DRect texture0;\n"
    "varying highp vec2 texcoord0, texcoord1, texcoord2, texcoord3;\n"
    "void main () {\n"
    "  lowp vec4 q0, q1, q2, q3;\n"
    "  q0 = texture2DRect(texture0, texcoord0);\n"
    "  q1 = texture2DRect(texture0, texcoord1);\n"
    "  q2 = texture2DRect(texture0, texcoord2);\n"
    "  q3 = texture2DRect(texture0, texcoord3);\n"
    "  gl_FragData[0] = (q0 + q1 + q2 + q3) * 0.25;\n"
    "}\n";

static int shadow_blur_given;

static void
give_shadow_blur(void)
{
	uintptr_t *entry = (uintptr_t *)(CA_SHADER_TABLE + slide + 8 * (uintptr_t)FUNCTION_SHADOW_BLUR);
	vm_address_t page = (vm_address_t)entry & ~(vm_address_t)(vm_page_size - 1);

	/* ds4 (26) must be where it was found: the table is the one this cache has */
	if (*(uintptr_t *)(CA_SHADER_TABLE + slide + 8 * 26) == 0 || *entry != 0) {
		say("hakios-ca: the shader table is not as expected; sb3 left alone\n");
		return;
	}
	if (vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
		say("hakios-ca: cannot make the table of shaders writable\n");
		return;
	}
	*entry = (uintptr_t)shadow_blur_source;
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
	shadow_blur_given = 1;
	say("hakios-ca: CoreAnimation's function 40 (\"sb3\") is given a shader of ours\n");
}

static int (*orig_function_supported)(void *context);
static void (*orig_function_uniform)(void *context, unsigned index, size_t components, size_t count, const float *values);

static int
my_function_supported(void *context)
{
	static uint8_t said[256];
	int supported = orig_function_supported(context);
	uint8_t function = *(*(uint8_t **)((char *)context + 0x10) + 0x10);

	if (!supported && function == FUNCTION_SHADOW_BLUR && shadow_blur_given) {
		return 1;
	}
	if (!supported) {
		hakios_gl_function_refused(function);
		if (!said[function]) {
			said[function] = 1;
			say("hakios-ca: CoreAnimation's OpenGL ES renderer has no function %u: what needs it is not drawn\n", function);
		}
	}
	return supported;
}

static void
my_function_uniform(void *context, unsigned index, size_t components, size_t count, const float *values)
{
	uint8_t function = *(*(uint8_t **)((char *)context + 0x10) + 0x10);

	hakios_gl_function_values(context, function, index, components, count, values);
	orig_function_uniform(context, index, components, count, values);
}

__attribute__((constructor)) static void
hakios_ca_hooks(void)
{
	size_t length = 0;
	const void *cache;

	if (strcmp(getprogname(), "backboardd") != 0 || (cache = _dyld_get_shared_cache_range(&length)) == NULL) {
		return;
	}
	slide = (uintptr_t)cache - CACHE_BASE;
	{
		char arguments[1024] = "";
		size_t size = sizeof(arguments) - 1;

		if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 && strstr(arguments, "glsb3=1") != NULL) {
			give_shadow_blur();
		}
	}
	orig_function_supported = replace_virtual("function_supported", VSLOT(VT_GLES_CONTEXT, 0x378), FUNCTION_SUPPORTED,
	    my_function_supported);
	orig_function_uniform = replace_virtual("function_uniform", VSLOT(VT_GLES_CONTEXT, 0x388), FUNCTION_UNIFORM,
	    my_function_uniform);
	say("hakios-ca: CoreAnimation hooks: function_supported %s, function_uniform %s (slide %#lx)\n",
	    orig_function_supported ? "watched" : "NOT watched", orig_function_uniform ? "watched" : "NOT watched", slide);
}
