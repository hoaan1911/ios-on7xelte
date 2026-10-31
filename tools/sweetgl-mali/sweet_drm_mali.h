/* tools/sweetgl-mali: Panfrost DRM emulation (sweet_drm_mali.c) over the J7 Mali user client. */
#ifndef SWEET_DRM_MALI_H
#define SWEET_DRM_MALI_H

#include <stddef.h>
#include <stdint.h>

/* A "DRM fd" to hand to Mesa (a dup of /dev/null), or -1 without the kernel's GPU service. */
int sweet_drm_open(void);

/* Replaces Panfrost's mmap of a BO (the BO memory already exists in user space): the BO behind a
 * MMAP_BO offset, or MAP_FAILED. */
void *sweet_drm_bo_map(int fd, uint64_t offset);

/* Replaces Panfrost's munmap of a BO mapping: the memory belongs to the BO until it is closed. */
int sweet_drm_bo_unmap(void *addr, size_t len);

/* printf-style line to the kernel log (selector 5; reaches the persistent log). */
void sweet_drm_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
