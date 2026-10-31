/*
 * libdrm + Panfrost DRM emulation for Mesa's panfrost gallium driver on iOS (no DRM here).
 *
 * Mesa 24.3 reaches the kernel through src/panfrost/lib/kmod/panfrost_kmod.c (drmIoctl with the
 * DRM_IOCTL_PANFROST_* requests), pan_kmod.c (drmGetVersion, drmPrimeFDToHandle), pan_bo.c, pan_jm.c
 * and pan_fence.c (drmSyncobj*), plus one mmap of a BO (pan_kmod.h, hooked to sweet_drm_bo_map by
 * the patch the mesa-build workflow applies). This file answers them in user space on top of the
 * kernel's J7MaliUserClient:
 *
 *   CREATE_BO       anonymous user memory, mapped into the GPU's address space by selector 0
 *                   (the kernel wires it and picks the GPU VA); handle -> {memory, GPU VA}
 *   GET_BO_OFFSET   the GPU VA
 *   MMAP_BO         handle << 32; sweet_drm_bo_map() turns it back into the memory
 *   SUBMIT          selector 2 (slot 0 = fragment job chains, 1 = vertex/tiler/compute) then
 *                   selector 3: every submit runs to completion before the ioctl returns, so
 *                   in-order submits keep their dependencies and every sync object is signalled
 *   WAIT_BO         nothing is ever in flight after a submit: always ready
 *   GET_PARAM       the GPU's own identity registers through selector 6
 *   drmSyncobj*     user-space objects, always signalled (see SUBMIT)
 *
 * One device per process; the "fd" is /dev/null (Mesa dup()s and closes it).
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>

#include "xf86drm.h"
#include "drm-uapi/panfrost_drm.h"
#include "sweet_drm_mali.h"

#define GPU_PAGE   0x4000u          /* the kernel wires whole CPU pages (16 KiB) */
#define MAX_BOS    4096u
#define MAX_SYNCOBJ 1024u

struct sbo {
    void *mem;
    uint64_t gva, size;
    int used;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static io_connect_t conn;
static int dev_fd = -1;
static struct sbo bos[MAX_BOS];
static uint8_t syncobj_used[MAX_SYNCOBJ];
static uint8_t syncobj_signaled[MAX_SYNCOBJ];
static uint32_t gpu_id;

static uint64_t id_reg[0x140 / 4];  /* identity registers read once, indexed by offset / 4 */
static uint8_t id_valid[0x140 / 4];

/* Identity registers the kernel lets us read (selector 6). */
static const uint32_t id_offsets[] = {
    0x000, 0x004, 0x008, 0x00c, 0x010, 0x014, 0x018, 0x01c, 0x0a0, 0x0a4, 0x0a8, 0x0ac,
    0x0b0, 0x0b4, 0x0b8, 0x0bc, 0x0c0, 0x0c4, 0x0c8, 0x0cc, 0x0d0, 0x0d4, 0x0d8, 0x0dc,
    0x0e0, 0x0e4, 0x0e8, 0x0ec, 0x0f0, 0x0f4, 0x0f8, 0x0fc, 0x100, 0x104, 0x110, 0x114,
    0x120, 0x124,
};

void
sweet_drm_log(const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (conn) {
        IOConnectCallStructMethod(conn, 5, line, (size_t)strlen(line), NULL, NULL);
    }
}

/* Lock held. */
static void
connect_dev(void)
{
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("A5XELTEGPU"));
    if (svc == 0 || IOServiceOpen(svc, mach_task_self(), 0, &conn) != KERN_SUCCESS) {
        conn = 0;
        return;
    }
    IOObjectRelease(svc);
    uint64_t out[6];
    uint32_t n = 6;
    if (IOConnectCallScalarMethod(conn, 4, NULL, 0, out, &n) != KERN_SUCCESS || n != 6 || (out[0] >> 16) != 0x830) {
        IOServiceClose(conn);
        conn = 0;
        return;
    }
    gpu_id = (uint32_t)out[0];
    for (unsigned i = 0; i < sizeof(id_offsets) / sizeof(id_offsets[0]); i++) {
        uint64_t in = id_offsets[i], v = 0;
        uint32_t m = 1;
        if (IOConnectCallScalarMethod(conn, 6, &in, 1, &v, &m) == KERN_SUCCESS && m == 1) {
            id_reg[id_offsets[i] / 4] = v;
            id_valid[id_offsets[i] / 4] = 1;
        }
    }
    dev_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
}

int
sweet_drm_open(void)
{
    pthread_mutex_lock(&lock);
    if (!conn) {
        connect_dev();
    }
    int fd = conn && dev_fd >= 0 ? dup(dev_fd) : -1;
    pthread_mutex_unlock(&lock);
    return fd;
}

int
drmOpenWithType(const char *name, const char *busid, int type)
{
    (void)busid;
    (void)type;
    return name && strcmp(name, "panfrost") == 0 ? sweet_drm_open() : -1;
}

/* ---- BO table (lock held for the helpers) ---- */
static struct sbo *
bo_get(uint32_t handle)
{
    return handle >= 1 && handle <= MAX_BOS && bos[handle - 1].used ? &bos[handle - 1] : NULL;
}

static int
bo_create(uint32_t size, uint32_t *handle, uint64_t *gva)
{
    uint64_t len = ((uint64_t)size + GPU_PAGE - 1) & ~(uint64_t)(GPU_PAGE - 1);
    if (size == 0 || len > (64ull << 20)) {
        return -EINVAL;
    }
    uint32_t h = 0;
    for (uint32_t i = 0; i < MAX_BOS; i++) {
        if (!bos[i].used) {
            h = i + 1;
            break;
        }
    }
    if (!h) {
        return -ENOMEM;
    }
    void *mem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) {
        return -ENOMEM;
    }
    memset(mem, 0, len);
    uint64_t in[2] = { (uint64_t)(uintptr_t)mem, len }, out[1] = { 0 };
    uint32_t n = 1;
    if (IOConnectCallScalarMethod(conn, 0, in, 2, out, &n) != KERN_SUCCESS || out[0] == 0) {
        munmap(mem, len);
        return -ENOMEM;
    }
    bos[h - 1] = (struct sbo){ .mem = mem, .gva = out[0], .size = len, .used = 1 };
    *handle = h;
    *gva = out[0];
    return 0;
}

static void
bo_close(uint32_t handle)
{
    struct sbo *b = bo_get(handle);
    if (!b) {
        return;
    }
    uint64_t in = b->gva;
    IOConnectCallScalarMethod(conn, 1, &in, 1, NULL, NULL);
    munmap(b->mem, b->size);
    memset(b, 0, sizeof(*b));
}

void *
sweet_drm_bo_map(int fd, uint64_t offset)
{
    (void)fd;
    void *p = MAP_FAILED;
    pthread_mutex_lock(&lock);
    struct sbo *b = bo_get((uint32_t)(offset >> 32));
    if (b) {
        p = b->mem;
    }
    pthread_mutex_unlock(&lock);
    return p;
}

int
sweet_drm_bo_unmap(void *addr, size_t len)
{
    (void)addr;
    (void)len;
    return 0;
}

void
drmCloseBufferHandle(int fd, uint32_t handle)
{
    (void)fd;
    pthread_mutex_lock(&lock);
    bo_close(handle);
    pthread_mutex_unlock(&lock);
}

/* ---- GET_PARAM ---- */
static int
get_param(uint32_t param, uint64_t *value)
{
    uint32_t reg = 0;
    switch (param) {
    case DRM_PANFROST_PARAM_GPU_PROD_ID: *value = gpu_id >> 16; return 0;
    case DRM_PANFROST_PARAM_GPU_REVISION: *value = gpu_id & 0xffff; return 0;
    case DRM_PANFROST_PARAM_SHADER_PRESENT: reg = 0x100; break;
    case DRM_PANFROST_PARAM_TILER_PRESENT: reg = 0x110; break;
    case DRM_PANFROST_PARAM_L2_PRESENT: reg = 0x120; break;
    case DRM_PANFROST_PARAM_AS_PRESENT: reg = 0x018; break;
    case DRM_PANFROST_PARAM_JS_PRESENT: reg = 0x01c; break;
    case DRM_PANFROST_PARAM_L2_FEATURES: reg = 0x004; break;
    case DRM_PANFROST_PARAM_CORE_FEATURES: reg = 0x008; break;
    case DRM_PANFROST_PARAM_TILER_FEATURES: reg = 0x00c; break;
    case DRM_PANFROST_PARAM_MEM_FEATURES: reg = 0x010; break;
    case DRM_PANFROST_PARAM_MMU_FEATURES: reg = 0x014; break;
    case DRM_PANFROST_PARAM_THREAD_FEATURES: reg = 0x0ac; break;
    case DRM_PANFROST_PARAM_MAX_THREADS: reg = 0x0a0; break;
    case DRM_PANFROST_PARAM_THREAD_MAX_WORKGROUP_SZ: reg = 0x0a4; break;
    case DRM_PANFROST_PARAM_THREAD_MAX_BARRIER_SZ: reg = 0x0a8; break;
    case DRM_PANFROST_PARAM_STACK_PRESENT:
    case DRM_PANFROST_PARAM_COHERENCY_FEATURES:
    case DRM_PANFROST_PARAM_THREAD_TLS_ALLOC:
    case DRM_PANFROST_PARAM_AFBC_FEATURES:
        *value = 0;     /* Bifrost+ or coherent-bus features: none on a T830 */
        return 0;
    case DRM_PANFROST_PARAM_NR_CORE_GROUPS: {
        uint64_t l2 = id_valid[0x120 / 4] ? id_reg[0x120 / 4] : 1;
        *value = (uint64_t)__builtin_popcountll(l2);
        return 0;
    }
    default:
        if (param >= DRM_PANFROST_PARAM_TEXTURE_FEATURES0 && param <= DRM_PANFROST_PARAM_TEXTURE_FEATURES3) {
            reg = 0x0b0 + 4 * (param - DRM_PANFROST_PARAM_TEXTURE_FEATURES0);
        } else if (param >= DRM_PANFROST_PARAM_JS_FEATURES0 && param <= DRM_PANFROST_PARAM_JS_FEATURES15) {
            reg = 0x0c0 + 4 * (param - DRM_PANFROST_PARAM_JS_FEATURES0);
        } else {
            return -EINVAL;   /* system timestamp and anything newer: optional for Mesa */
        }
    }
    if (!id_valid[reg / 4]) {
        return -EINVAL;
    }
    *value = id_reg[reg / 4];
    return 0;
}

/* ---- SUBMIT: run the job chain to completion ---- */
static int
submit(const struct drm_panfrost_submit *s)
{
    const uint32_t *handles = (const uint32_t *)(uintptr_t)s->bo_handles;
    for (uint32_t i = 0; i < s->bo_handle_count; i++) {
        if (!bo_get(handles[i])) {
            return -ENOENT;
        }
    }
    uint64_t slot = (s->requirements & PANFROST_JD_REQ_FS) ? 0 : 1;
    uint64_t in[2] = { slot, s->jc }, out[2] = { 0, 0 };
    uint32_t n = 1;
    if (IOConnectCallScalarMethod(conn, 2, in, 2, out, &n) != KERN_SUCCESS) {
        return -EINVAL;
    }
    uint64_t t = 5000000, w[2] = { 0, 0 };
    n = 2;
    if (IOConnectCallScalarMethod(conn, 3, &t, 1, w, &n) != KERN_SUCCESS) {
        return -EIO;
    }
    if ((int64_t)w[0] != 0) {
        char line[96];
        snprintf(line, sizeof(line), "submit jc=%llx slot=%llu failed status=%d js=%llx",
            (unsigned long long)s->jc, (unsigned long long)slot, (int)(int64_t)w[0], (unsigned long long)w[1]);
        IOConnectCallStructMethod(conn, 5, line, strlen(line), NULL, NULL);
        return -EIO;
    }
    if (s->out_sync && s->out_sync <= MAX_SYNCOBJ && syncobj_used[s->out_sync - 1]) {
        syncobj_signaled[s->out_sync - 1] = 1;
    }
    return 0;
}

int
drmIoctl(int fd, unsigned long request, void *arg)
{
    (void)fd;
    if (!conn || !arg) {
        errno = ENODEV;
        return -1;
    }
    int ret = -ENOTTY;
    pthread_mutex_lock(&lock);
    switch (request) {
    case DRM_IOCTL_PANFROST_GET_PARAM: {
        struct drm_panfrost_get_param *p = arg;
        ret = get_param(p->param, &p->value);
        break;
    }
    case DRM_IOCTL_PANFROST_CREATE_BO: {
        struct drm_panfrost_create_bo *c = arg;
        ret = bo_create(c->size, &c->handle, &c->offset);
        break;
    }
    case DRM_IOCTL_PANFROST_GET_BO_OFFSET: {
        struct drm_panfrost_get_bo_offset *g = arg;
        struct sbo *b = bo_get(g->handle);
        ret = b ? 0 : -ENOENT;
        if (b) {
            g->offset = b->gva;
        }
        break;
    }
    case DRM_IOCTL_PANFROST_MMAP_BO: {
        struct drm_panfrost_mmap_bo *m = arg;
        ret = bo_get(m->handle) ? 0 : -ENOENT;
        if (ret == 0) {
            m->offset = (uint64_t)m->handle << 32;
        }
        break;
    }
    case DRM_IOCTL_PANFROST_WAIT_BO: {
        struct drm_panfrost_wait_bo *w = arg;
        ret = bo_get(w->handle) ? 0 : -ENOENT;     /* nothing is in flight after a submit */
        break;
    }
    case DRM_IOCTL_PANFROST_MADVISE: {
        struct drm_panfrost_madvise *m = arg;
        ret = bo_get(m->handle) ? 0 : -ENOENT;
        if (ret == 0) {
            m->retained = 1;
        }
        break;
    }
    case DRM_IOCTL_PANFROST_SUBMIT:
        ret = submit(arg);
        break;
    case DRM_IOCTL_PANFROST_PERFCNT_ENABLE:
    case DRM_IOCTL_PANFROST_PERFCNT_DUMP:
        ret = -ENOSYS;
        break;
    default:
        break;
    }
    pthread_mutex_unlock(&lock);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return 0;
}

int
drmCommandWrite(int fd, unsigned long index, void *data, unsigned long size)
{
    (void)fd; (void)index; (void)data; (void)size;
    errno = ENOSYS;
    return -1;
}

int
drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size)
{
    (void)fd; (void)index; (void)data; (void)size;
    errno = ENOSYS;
    return -1;
}

drmVersionPtr
drmGetVersion(int fd)
{
    (void)fd;
    drmVersionPtr v = calloc(1, sizeof(*v));
    if (!v) {
        return NULL;
    }
    v->version_major = 1;
    v->version_minor = 3;
    v->name = strdup("panfrost");
    v->date = strdup("20180908");
    v->desc = strdup("Panfrost GPU over the J7 Mali user client");
    v->name_len = (int)strlen(v->name);
    v->date_len = (int)strlen(v->date);
    v->desc_len = (int)strlen(v->desc);
    return v;
}

void
drmFreeVersion(drmVersionPtr v)
{
    if (v) {
        free(v->name);
        free(v->date);
        free(v->desc);
        free(v);
    }
}

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
    (void)fd; (void)capability;
    *value = 0;
    errno = EINVAL;
    return -1;
}

/* dma-buf sharing does not exist here: IOSurface import is the EAGL layer's job. */
int
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
    (void)fd; (void)handle; (void)flags; (void)prime_fd;
    return -ENOSYS;
}

int
drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
    (void)fd; (void)prime_fd; (void)handle;
    return -ENOSYS;
}

/* ---- sync objects: always signalled once created signalled or used as an out_sync ---- */
int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
    (void)fd;
    int ret = -ENOMEM;
    pthread_mutex_lock(&lock);
    for (uint32_t i = 0; i < MAX_SYNCOBJ; i++) {
        if (!syncobj_used[i]) {
            syncobj_used[i] = 1;
            syncobj_signaled[i] = (flags & 1) ? 1 : 0;   /* DRM_SYNCOBJ_CREATE_SIGNALED */
            *handle = i + 1;
            ret = 0;
            break;
        }
    }
    pthread_mutex_unlock(&lock);
    return ret;
}

int
drmSyncobjDestroy(int fd, uint32_t handle)
{
    (void)fd;
    int ret = -ENOENT;
    pthread_mutex_lock(&lock);
    if (handle >= 1 && handle <= MAX_SYNCOBJ && syncobj_used[handle - 1]) {
        syncobj_used[handle - 1] = syncobj_signaled[handle - 1] = 0;
        ret = 0;
    }
    pthread_mutex_unlock(&lock);
    return ret;
}

int
drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd)
{
    (void)fd; (void)handle;
    *obj_fd = dev_fd >= 0 ? dup(dev_fd) : -1;
    return *obj_fd >= 0 ? 0 : -ENODEV;
}

int
drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{
    (void)fd; (void)obj_fd; (void)handle;
    return -ENOSYS;
}

int
drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
    (void)fd; (void)sync_file_fd;
    int ret = -ENOENT;
    pthread_mutex_lock(&lock);
    if (handle >= 1 && handle <= MAX_SYNCOBJ && syncobj_used[handle - 1]) {
        syncobj_signaled[handle - 1] = 1;     /* a sync file from elsewhere has long completed */
        ret = 0;
    }
    pthread_mutex_unlock(&lock);
    return ret;
}

int
drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
    (void)fd; (void)handle;
    *sync_file_fd = dev_fd >= 0 ? dup(dev_fd) : -1;   /* never waited on by anyone here */
    return *sync_file_fd >= 0 ? 0 : -ENODEV;
}

int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count)
{
    (void)fd;
    pthread_mutex_lock(&lock);
    for (uint32_t i = 0; i < handle_count; i++) {
        if (handles[i] >= 1 && handles[i] <= MAX_SYNCOBJ) {
            syncobj_signaled[handles[i] - 1] = 0;
        }
    }
    pthread_mutex_unlock(&lock);
    return 0;
}

int
drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles, int64_t timeout_nsec,
    unsigned flags, uint32_t *first_signaled)
{
    (void)fd; (void)timeout_nsec;
    /* flags: 1 = WAIT_ALL, 2 = WAIT_FOR_SUBMIT. Everything that was ever submitted already ran. */
    int ret = 0, any = 0;
    pthread_mutex_lock(&lock);
    for (unsigned i = 0; i < num_handles; i++) {
        if (handles[i] < 1 || handles[i] > MAX_SYNCOBJ || !syncobj_used[handles[i] - 1]) {
            ret = -ENOENT;
            break;
        }
        if (syncobj_signaled[handles[i] - 1]) {
            if (!any && first_signaled) {
                *first_signaled = i;
            }
            any = 1;
        } else if (flags & 1) {
            ret = -ETIME;
        }
    }
    if (ret == 0 && !any) {
        ret = -ETIME;
    }
    pthread_mutex_unlock(&lock);
    return ret;
}

int
drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned num_handles,
    int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
    (void)points;
    return drmSyncobjWait(fd, handles, num_handles, timeout_nsec, flags, first_signaled);
}
