// SPDX-License-Identifier: MIT
// J7 Prime port: the OpenGL ES library behind HakiOS's eagl.c (hakios.gl.h): our libOSMesa, which is Mesa
// with Panfrost on the Mali-T830 over the kernel's GPU service (tools/sweetgl-mali: sweet_hw.c,
// sweet_drm_mali.c) or softpipe when there is none. Adapted from the Galaxy S7 port's gl_hero.c
// (ios-a5xelte-herolte, gpu/hakios-gl/src/gl_hero.c), which replaces HakiOS's gl_osmesa.c.
//
// Not ported: the S7's zero-copy path (an IOSurface's memory becoming a texture's, OSMesaSweet*). Here
// hakios_gl_surface_texture() says "not done", which eagl.c supports: the texture is filled and read
// back with OpenGL ES calls, as with glcopy=1.
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "hakios.gl.h"

#define OSMESA_RGBA                     0x1908
#define OSMESA_FORMAT                   0x22
#define OSMESA_DEPTH_BITS               0x30
#define OSMESA_STENCIL_BITS             0x31
#define OSMESA_PROFILE                  0x33
#define OSMESA_CONTEXT_MAJOR_VERSION    0x36
#define OSMESA_CONTEXT_MINOR_VERSION    0x37
#define OSMESA_ES_PROFILE_SWEET         0x36    /* tools/sweetgl-mali/patch-osmesa.py */
#define GL_UNSIGNED_BYTE                0x1401

typedef struct osmesa_context *OSMesaContext;
extern OSMesaContext OSMesaCreateContextAttribs(const int *attributes, OSMesaContext share);
extern unsigned char OSMesaMakeCurrent(OSMesaContext context, void *buffer, unsigned type, int width, int height);
extern void OSMesaDestroyContext(OSMesaContext context);
extern int sweet_hw_active(void);       /* sweet_hw.c: the Panfrost screen is the one in use */

#define WINDOW  16

struct hakios_gl {
	OSMesaContext context;
	unsigned char window[WINDOW * WINDOW * 4];
};

struct hakios_gl *
hakios_gl_create(int version)
{
	const int attributes[] = {
		OSMESA_FORMAT, OSMESA_RGBA, OSMESA_DEPTH_BITS, 24, OSMESA_STENCIL_BITS, 8,
		OSMESA_PROFILE, OSMESA_ES_PROFILE_SWEET, OSMESA_CONTEXT_MAJOR_VERSION, version, OSMESA_CONTEXT_MINOR_VERSION, 0,
		0
	};
	struct hakios_gl *gl = calloc(1, sizeof(*gl));

	if (gl != NULL && (gl->context = OSMesaCreateContextAttribs(attributes, NULL)) == NULL) {
		free(gl);
		gl = NULL;
	}
	return gl;
}

int
hakios_gl_make_current(struct hakios_gl *gl)
{
	if (gl == NULL) {
		return OSMesaMakeCurrent(NULL, NULL, 0, 0, 0);
	}
	return OSMesaMakeCurrent(gl->context, gl->window, GL_UNSIGNED_BYTE, WINDOW, WINDOW);
}

void
hakios_gl_destroy(struct hakios_gl *gl)
{
	OSMesaDestroyContext(gl->context);
	free(gl);
}

const char *
hakios_gl_library(void)
{
	return sweet_hw_active() ? "Mesa Panfrost on the Mali-T830 (OSMesa, sweet_drm_mali)"
	                         : "Mesa softpipe (OSMesa), no GPU service";
}

int
hakios_gl_surface_texture(struct hakios_gl *gl, uint32_t surface)
{
	(void)gl;
	(void)surface;
	return 0;
}

void
hakios_gl_surface_written(struct hakios_gl *gl, uint32_t surface)
{
	(void)gl;
	(void)surface;
}
