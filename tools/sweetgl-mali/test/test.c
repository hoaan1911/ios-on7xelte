#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <IOKit/IOKitLib.h>
#include "xf86drm.h"
#include "drm-uapi/panfrost_drm.h"
#include "sweet_drm_mali.h"

/* ---------------- a fake kernel J7MaliUserClient: the same selectors and semantics ---------------- */
struct kmap { uint64_t user, gva, len; int used; } kmaps[64]; static uint64_t next_gva = 0x01000000; static int selftrace;
static int kfault_next;
static int kseq; static int klast_status;
mach_port_t mach_task_self(void) { return 1; }
void *IOServiceMatching(const char *n) { return (void *)n; }
io_service_t IOServiceGetMatchingService(mach_port_t p, void *m) { (void)p; return strcmp((const char *)m, "A5XELTEGPU") == 0 ? 7 : 0; }
kern_return_t IOServiceOpen(io_service_t s, mach_port_t t, uint32_t ty, io_connect_t *c) { (void)s; (void)t; (void)ty; *c = 9; return 0; }
kern_return_t IOServiceClose(io_connect_t c) { (void)c; return 0; }
kern_return_t IOObjectRelease(io_object_t o) { (void)o; return 0; }
static void *g2p(uint64_t gva) { for (int i = 0; i < 64; i++) if (kmaps[i].used && gva >= kmaps[i].gva && gva < kmaps[i].gva + kmaps[i].len) return (void *)(uintptr_t)(kmaps[i].user + (gva - kmaps[i].gva)); return NULL; }
static const struct { uint32_t off, val; } idregs[] = { {0x000,0x08300010},{0x004,0x07100206},{0x008,0x00000000},{0x00c,0x00000809},{0x010,1},{0x014,0x2821},{0x018,0xff},{0x01c,7},
    {0x0a0,0x100},{0x0a4,0x100},{0x0a8,0x100},{0x0ac,0x0a040400},{0x0b0,0xfffe003f},{0x0b4,0xdead0001},{0x0b8,0xdead0002},{0x0bc,0xdead0003},{0x0c0,0x20e},{0x0c4,0x1fe},{0x0c8,0x7e},
    {0x100,1},{0x110,1},{0x120,1} };
kern_return_t IOConnectCallScalarMethod(io_connect_t c, uint32_t sel, const uint64_t *in, uint32_t nin, uint64_t *out, uint32_t *nout) {
    (void)c; (void)nin;
    switch (sel) {
    case 0: for (int i = 0; i < 64; i++) if (!kmaps[i].used) { assert(in[0] % 4096 == 0 && in[1] % 16384 == 0); kmaps[i] = (struct kmap){ in[0], next_gva, in[1], 1 }; out[0] = next_gva; *nout = 1; next_gva += in[1] + 4096; return 0; } return 5;
    case 1: for (int i = 0; i < 64; i++) if (kmaps[i].used && kmaps[i].gva == in[0]) { kmaps[i].used = 0; return 0; } return 5;
    case 2: { if (!g2p(in[1]) || in[0] > 2) return 0x2c2; uint64_t head = in[1]; kfault_next = 0;
        while (head) { uint8_t *job = g2p(head); if (!job) { kfault_next = 1; break; }
            if (((job[16] >> 1) & 0x7f) == 2 && *(uint64_t *)(job + 40) == 3) { uint64_t *o = g2p(*(uint64_t *)(job + 32)); if (!o) { kfault_next = 1; break; } *o = 0; } else { kfault_next = 1; break; }
            head = *(uint64_t *)(job + 24); }
        klast_status = kfault_next ? -3 : 0; out[0] = ++kseq; *nout = 1; return 0; }
    case 3: out[0] = (uint64_t)(int64_t)klast_status; out[1] = 0; *nout = 2; klast_status = 0; return 0;
    case 4: out[0] = 0x08300010; out[1] = 1; out[2] = 1; out[3] = 1; out[4] = 7; out[5] = 0xff; *nout = 6; return 0;
    case 6: for (unsigned i = 0; i < sizeof(idregs) / sizeof(idregs[0]); i++) if (idregs[i].off == in[0]) { out[0] = idregs[i].val; *nout = 1; return 0; } out[0] = 0; *nout = 1; return 0;
    }
    return 0x2c2;
}
kern_return_t IOConnectCallStructMethod(io_connect_t c, uint32_t sel, const void *in, size_t n, void *out, size_t *on) { (void)c; (void)out; (void)on; if (sel == 5) { selftrace++; printf("  [kernel log] %.*s\n", (int)n, (const char *)in); } return 0; }

/* ---------------- the test ---------------- */
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s (errno %d)\n", __LINE__, #c, errno); return 1; } } while (0)
struct wv { uint32_t ex, fi; uint64_t fp; uint8_t ts, fl; uint16_t idx, d1, d2; uint64_t next, out, vt; } __attribute__((packed));

int main(void) {
    int fd = sweet_drm_open(); CHECK(fd >= 0);
    drmVersionPtr v = drmGetVersion(fd); CHECK(v && strcmp(v->name, "panfrost") == 0 && v->version_major == 1); drmFreeVersion(v);

    struct drm_panfrost_get_param gp = { .param = DRM_PANFROST_PARAM_GPU_PROD_ID };
    CHECK(drmIoctl(fd, DRM_IOCTL_PANFROST_GET_PARAM, &gp) == 0 && gp.value == 0x830);
    gp.param = DRM_PANFROST_PARAM_GPU_REVISION; CHECK(drmIoctl(fd, DRM_IOCTL_PANFROST_GET_PARAM, &gp) == 0 && gp.value == 0x0010);
    gp.param = DRM_PANFROST_PARAM_THREAD_FEATURES; CHECK(drmIoctl(fd, DRM_IOCTL_PANFROST_GET_PARAM, &gp) == 0 && gp.value == 0x0a040400);
    gp.param = DRM_PANFROST_PARAM_TEXTURE_FEATURES0 + 2; CHECK(drmIoctl(fd, DRM_IOCTL_PANFROST_GET_PARAM, &gp) == 0 && gp.value == 0xdead0002);
    gp.param = DRM_PANFROST_PARAM_JS_FEATURES0 + 1; CHECK(drmIoctl(fd, DRM_IOCTL_PANFROST_GET_PARAM, &gp) == 0 && gp.value == 0x1fe);
    gp.param = DRM_PANFROST_PARAM_SHADER_PRESENT; CHECK(drmIoctl(fd, DRM_IOCTL_PANFROST_GET_PARAM, &gp) == 0 && gp.value == 1);
    gp.param = DRM_PANFROST_PARAM_NR_CORE_GROUPS; CHECK(drmIoctl(fd, DRM_IOCTL_PANFROST_GET_PARAM, &gp) == 0 && gp.value == 1);
    gp.param = DRM_PANFROST_PARAM_STACK_PRESENT; CHECK(drmIoctl(fd, DRM_IOCTL_PANFROST_GET_PARAM, &gp) == 0 && gp.value == 0);
    gp.param = DRM_PANFROST_PARAM_SYSTEM_TIMESTAMP; CHECK(drmIoctl(fd, DRM_IOCTL_PANFROST_GET_PARAM, &gp) == -1 && errno == EINVAL);

    struct drm_panfrost_create_bo a = { .size = 5000 }, b = { .size = 100000 };
    CHECK(drmIoctl(fd, DRM_IOCTL_PANFROST_CREATE_BO, &a) == 0 && a.handle && a.offset);
    CHECK(drmIoctl(fd, DRM_IOCTL_PANFROST_CREATE_BO, &b) == 0 && b.handle != a.handle && b.offset != a.offset);
    struct drm_panfrost_get_bo_offset go = { .handle = b.handle }; CHECK(drmIoctl(fd, DRM_IOCTL_PANFROST_GET_BO_OFFSET, &go) == 0 && go.offset == b.offset);
    struct drm_panfrost_mmap_bo ma = { .handle = a.handle }, mb = { .handle = b.handle };
    CHECK(drmIoctl(fd, DRM_IOCTL_PANFROST_MMAP_BO, &ma) == 0 && drmIoctl(fd, DRM_IOCTL_PANFROST_MMAP_BO, &mb) == 0 && ma.offset != mb.offset);
    uint8_t *pa = sweet_drm_bo_map(fd, ma.offset), *pb = sweet_drm_bo_map(fd, mb.offset);
    CHECK(pa != MAP_FAILED && pb != MAP_FAILED && pa != pb && sweet_drm_bo_map(fd, 77ull << 32) == MAP_FAILED);
    CHECK(sweet_drm_bo_unmap(pa, 5000) == 0);

    /* a vertex-style job chain in BO a (slot 1) writing zero into two words of BO b; then a fragment-style one (slot 0) */
    *(volatile uint64_t *)(pb + 0x10) = 0xa5a5a5a5a5a5a5a5ull; *(volatile uint64_t *)(pb + 0x20000 / 2) = 0xa5a5a5a5a5a5a5a5ull; *(volatile uint64_t *)(pb + 0x18) = 0x1111;
    struct wv *j0 = (struct wv *)pa, *j1 = (struct wv *)(pa + 0x100); memset(pa, 0, 0x200);
    j0->ts = 1 | (2 << 1); j0->idx = 1; j0->next = a.offset + 0x100; j0->out = b.offset + 0x10; j0->vt = 3;
    j1->ts = 1 | (2 << 1); j1->idx = 2; j1->out = b.offset + 0x20000 / 2; j1->vt = 3;
    uint32_t syncA = 0, syncB = 0; CHECK(drmSyncobjCreate(fd, 0, &syncA) == 0 && drmSyncobjCreate(fd, 0, &syncB) == 0 && syncA != syncB);
    uint32_t first; CHECK(drmSyncobjWait(fd, &syncA, 1, 0, 0, &first) == -ETIME);                    /* not submitted yet */
    uint32_t handles[2] = { a.handle, b.handle };
    struct drm_panfrost_submit s = { .jc = a.offset, .bo_handles = (uintptr_t)handles, .bo_handle_count = 2, .out_sync = syncA, .requirements = 0 };
    CHECK(drmIoctl(fd, DRM_IOCTL_PANFROST_SUBMIT, &s) == 0);
    CHECK(*(volatile uint64_t *)(pb + 0x10) == 0 && *(volatile uint64_t *)(pb + 0x10000) == 0 && *(volatile uint64_t *)(pb + 0x18) == 0x1111);
    CHECK(drmSyncobjWait(fd, &syncA, 1, 0, 1, &first) == 0);
    struct drm_panfrost_wait_bo wb = { .handle = b.handle, .timeout_ns = 1000 }; CHECK(drmIoctl(fd, DRM_IOCTL_PANFROST_WAIT_BO, &wb) == 0);
    *(volatile uint64_t *)(pb + 0x10) = 7; s.requirements = PANFROST_JD_REQ_FS; s.out_sync = syncB; j0->next = 0;
    CHECK(drmIoctl(fd, DRM_IOCTL_PANFROST_SUBMIT, &s) == 0 && *(volatile uint64_t *)(pb + 0x10) == 0);   /* fragment slot too */
    CHECK(drmSyncobjWait(fd, &syncB, 1, 0, 1, &first) == 0);
    uint32_t both[2] = { syncA, syncB }; CHECK(drmSyncobjWait(fd, both, 2, 0, 1, &first) == 0);
    CHECK(drmSyncobjReset(fd, &syncB, 1) == 0 && drmSyncobjWait(fd, &syncB, 1, 0, 0, &first) == -ETIME);

    /* error paths */
    handles[1] = 999; CHECK(drmIoctl(fd, DRM_IOCTL_PANFROST_SUBMIT, &s) == -1 && errno == ENOENT); handles[1] = b.handle;
    j0->out = 0x7ff00000ull; s.out_sync = 0; CHECK(drmIoctl(fd, DRM_IOCTL_PANFROST_SUBMIT, &s) == -1 && errno == EIO);   /* GPU fault -> EIO, not a hang */
    struct drm_panfrost_madvise md = { .handle = a.handle, .madv = PANFROST_MADV_WILLNEED }; CHECK(drmIoctl(fd, DRM_IOCTL_PANFROST_MADVISE, &md) == 0 && md.retained == 1);
    CHECK(drmIoctl(fd, DRM_IOCTL_PANFROST_PERFCNT_ENABLE, &md) == -1 && errno == ENOSYS);
    CHECK(drmIoctl(fd, 0xdeadbeef, &md) == -1 && errno == ENOTTY);
    CHECK(drmPrimeFDToHandle(fd, 5, &first) == -ENOSYS);

    drmCloseBufferHandle(fd, a.handle); int still = 0; for (int i = 0; i < 64; i++) still += kmaps[i].used; CHECK(still == 1);   /* only b stays mapped */
    CHECK(sweet_drm_bo_map(fd, ma.offset) == MAP_FAILED);
    drmCloseBufferHandle(fd, b.handle); still = 0; for (int i = 0; i < 64; i++) still += kmaps[i].used; CHECK(still == 0);
    struct drm_panfrost_create_bo c = { .size = 4096 }; CHECK(drmIoctl(fd, DRM_IOCTL_PANFROST_CREATE_BO, &c) == 0 && c.handle == a.handle);   /* handle reuse */
    sweet_drm_log("emulation test done, %d", 42); CHECK(selftrace >= 2);
    close(fd);
    printf("ALL CHECKS PASSED\n");
    return 0;
}
