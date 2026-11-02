/*
 * OSMesa on the J7 Prime's Mali-T830: osmesa_create_screen() (patched by patch-osmesa.py to try this
 * first) gets a Panfrost screen over the libdrm stand-in (sweet_drm_mali.c) and the kernel's GPU
 * service, instead of softpipe. SWEET_GPU=0 keeps softpipe; so does a kernel without the service.
 *
 * Adapted from the Galaxy S7 port's hero_hw_create_screen (gpu/mesa/hero_hw.c, ios-a5xelte-herolte).
 * The S7's IOSurface render-target half (zero-copy blits into CoreAnimation's surfaces) is not ported:
 * without it the EAGL layer renders on the GPU and reads the result back with the CPU.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/driconf.h"
#include "util/macros.h"
#include "util/xmlconfig.h"
#include "panfrost/drm/panfrost_drm_public.h"
#include "xf86drm.h"

void sweet_drm_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));   /* sweet_drm_mali.c */

struct pipe_screen *sweet_hw_create_screen(void);
int sweet_hw_active(void);

static struct pipe_screen *sweet_screen;

static const driOptionDescription sweet_driconf[] = {
#include "panfrost/driinfo_panfrost.h"
};

/* Panfrost reads its options from these for the screen's lifetime. */
static driOptionCache options, options_info;

struct pipe_screen *
sweet_hw_create_screen(void)
{
   const char *env = getenv("SWEET_GPU");
   if (env && env[0] == '0') {
      sweet_drm_log("sweet_hw: SWEET_GPU=0, using softpipe");
      return NULL;
   }

   int fd = drmOpenWithType("panfrost", NULL, 0);
   if (fd < 0) {
      sweet_drm_log("sweet_hw: no GPU service in this kernel, using softpipe");
      return NULL;
   }

   driParseOptionInfo(&options_info, sweet_driconf, ARRAY_SIZE(sweet_driconf));
   driParseConfigFiles(&options, &options_info, 0, "panfrost", NULL, NULL, NULL, 0, NULL, 0);
   struct pipe_screen_config config = {
      .options = &options,
      .options_info = &options_info,
   };
   struct pipe_screen *screen = panfrost_drm_screen_create(fd, &config);
   /* The screen holds its own dup of the descriptor. */
   close(fd);
   if (!screen)
      sweet_drm_log("sweet_hw: Panfrost screen creation failed, using softpipe");
   else
      sweet_drm_log("sweet_hw: Panfrost screen created");
   sweet_screen = screen;
   return screen;
}

int
sweet_hw_active(void)
{
   return sweet_screen != NULL;
}
