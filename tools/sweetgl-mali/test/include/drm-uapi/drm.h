/* host-test stand-in for Mesa's include/drm-uapi/drm.h: just what panfrost_drm.h and xf86drm.h need */
#ifndef HT_DRM_H
#define HT_DRM_H
#include <stdint.h>
#include <sys/ioctl.h>
typedef uint8_t __u8; typedef uint16_t __u16; typedef uint32_t __u32; typedef uint64_t __u64; typedef int32_t __s32; typedef int64_t __s64;
#define DRM_IOCTL_BASE 'd'
#define DRM_IO(nr) _IO(DRM_IOCTL_BASE, nr)
#define DRM_IOR(nr, type) _IOR(DRM_IOCTL_BASE, nr, type)
#define DRM_IOW(nr, type) _IOW(DRM_IOCTL_BASE, nr, type)
#define DRM_IOWR(nr, type) _IOWR(DRM_IOCTL_BASE, nr, type)
#define DRM_COMMAND_BASE 0x40
#endif
