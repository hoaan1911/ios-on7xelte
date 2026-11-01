/* Symbols Mesa's gallium libraries reference on paths this port never takes. The link check
 * (mesa-build.yml) listed exactly these four once the shared GL dispatch (glapi) objects are
 * included; each one is only reachable from code that needs a feature we do not have:
 *
 *   pipe_loader_drm_probe_fd                 vl_drm_screen_create: the video layer's DRM winsys
 *   renderonly_create_gpu_import_for_resource
 *   renderonly_scanout_destroy               panfrost_resource_from_handle / _destroy: only when the
 *                                            screen has a renderonly (kmsro) scanout device, which
 *                                            sweet_drm_mali never creates (ro stays NULL)
 *   sw_screen_create_vk                      pipe_loader_sw: the software Vulkan screen
 *
 * The prototypes are loose on purpose (pointers are pointers on arm64): the real ones need Mesa's
 * headers and these functions must never run. If one does, it fails the way "unsupported" does. */
#include <stdbool.h>
#include <stddef.h>

bool
pipe_loader_drm_probe_fd(void **dev, int fd, bool zink)
{
    (void)dev;
    (void)fd;
    (void)zink;
    return false;
}

void *
renderonly_create_gpu_import_for_resource(void *rsc, void *ro, void *out_handle)
{
    (void)rsc;
    (void)ro;
    (void)out_handle;
    return NULL;
}

void
renderonly_scanout_destroy(void *scanout, void *ro)
{
    (void)scanout;
    (void)ro;
}

void *
sw_screen_create_vk(void *winsys, const void *config, bool sw_vk)
{
    (void)winsys;
    (void)config;
    (void)sw_vk;
    return NULL;
}
