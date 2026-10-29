/*
 * j7gputest: loaded into backboardd with
 *     sweetenv=backboardd:DYLD_INSERT_LIBRARIES=/usr/lib/sweetgl.dylib
 * It opens the kernel's J7MaliUserClient (boot-arg j7gpu=7), runs the same checks the
 * kernel-side client selftest runs (info, map user memory, a good job chain, a deliberate fault
 * and the recovery, a good chain again, unowned VA refused, unmap) from a real iOS process, and
 * reports every step through selector 5 into the kernel log (the persistent log, a5log=1).
 * It is the first real user-space program on the J7's GPU path; Mesa comes after it.
 *
 * The file is built as /usr/lib/sweetgl.dylib and padded to 64 MiB: that is the placeholder
 * the root image reserves, so tools/j7-push-lib.py can rewrite it in place.
 */
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#pragma pack(push, 1)
struct write_value_job {
    uint32_t exceptionStatus;
    uint32_t firstIncompleteTask;
    uint64_t faultPointer;
    uint8_t typeAndSize;
    uint8_t flags;
    uint16_t index;
    uint16_t dependency1;
    uint16_t dependency2;
    uint64_t next;
    uint64_t outputVA;
    uint64_t valueType;
};
#pragma pack(pop)
_Static_assert(sizeof(struct write_value_job) == 48, "job size");

static io_connect_t conn;

static void klog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void
klog(const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    IOConnectCallStructMethod(conn, 5, line, (size_t)strlen(line), NULL, NULL);
}

static kern_return_t
call(uint32_t sel, const uint64_t *in, uint32_t nin, uint64_t *out, uint32_t nout)
{
    uint32_t n = nout;
    return IOConnectCallScalarMethod(conn, sel, in, nin, out, nout ? &n : NULL);
}

static void *
test_thread(void *unused)
{
    (void)unused;
    sleep(8);   /* let backboardd finish starting first */
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("A5XELTEGPU"));
    if (svc == 0) {
        return NULL;
    }
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) {
        return NULL;
    }
    int ok = 1;
    uint64_t in[3] = { 0 }, out[6] = { 0 };
    kr = call(4, in, 0, out, 6);
    klog("dylib: pid %d info kr=%x gpu_id=%08llx shader=%llx tiler=%llx l2=%llx js=%llx as=%llx",
        getpid(), kr, out[0], out[1], out[2], out[3], out[4], out[5]);
    ok = ok && kr == KERN_SUCCESS && (out[0] >> 16) == 0x830;

    const size_t len = 0x10000;
    uint8_t *mem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) {
        klog("dylib: mmap failed");
        return NULL;
    }
    memset(mem, 0, len);
    in[0] = (uint64_t)(uintptr_t)mem;
    in[1] = len;
    kr = call(0, in, 2, out, 1);
    const uint64_t gva = out[0];
    klog("dylib: map kr=%x gva=%llx", kr, gva);
    ok = ok && kr == KERN_SUCCESS && gva != 0;

    for (unsigned round = 0; ok && round < 3; round++) {
        const int faulty = round == 1;
        memset(mem, 0, 0x200);
        struct write_value_job *j0 = (struct write_value_job *)mem;
        struct write_value_job *j1 = (struct write_value_job *)(mem + 0x100);
        *(volatile uint64_t *)(mem + 0x2008) = 0xa5a5a5a55a5a5a5aULL;
        *(volatile uint64_t *)(mem + 0x9010) = 0xa5a5a5a55a5a5a5aULL;
        j0->typeAndSize = 1 | (2 << 1);
        j0->index = 1;
        j0->next = faulty ? 0 : gva + 0x100;
        j0->outputVA = faulty ? 0x7ff00000ULL : gva + 0x2008;
        j0->valueType = 3;
        j1->typeAndSize = 1 | (2 << 1);
        j1->index = 2;
        j1->outputVA = gva + 0x9010;
        j1->valueType = 3;
        in[0] = 1;
        in[1] = gva;
        kr = call(2, in, 2, out, 1);
        in[0] = 200000;
        kern_return_t kw = call(3, in, 1, out, 2);
        const int rc = (int)(int64_t)out[0];
        const uint64_t a = *(volatile uint64_t *)(mem + 0x2008), b = *(volatile uint64_t *)(mem + 0x9010);
        klog("dylib: round %u (%s) submit=%x wait=%x status=%d out2008=%016llx out9010=%016llx",
            round, faulty ? "fault" : "good", kr, kw, rc, a, b);
        ok = faulty ? (kr == KERN_SUCCESS && kw == KERN_SUCCESS && (rc == -3 || rc == -2))
                    : (kr == KERN_SUCCESS && kw == KERN_SUCCESS && rc == 0 && a == 0 && b == 0);
    }
    in[0] = 1;
    in[1] = 0x7ff00000ULL;
    kr = call(2, in, 2, out, 1);
    klog("dylib: submit of an unowned VA -> %x (must be an error)", kr);
    ok = ok && kr != KERN_SUCCESS;
    in[0] = gva;
    kr = call(1, in, 1, out, 0);
    klog("dylib: unmap kr=%x", kr);
    ok = ok && kr == KERN_SUCCESS;
    munmap(mem, len);
    klog("dylib: j7gputest %s", ok ? "PASSED" : "FAILED");
    return NULL;
}

__attribute__((constructor)) static void
j7gputest_init(void)
{
    const char *name = getprogname();
    if (!name || strcmp(name, "backboardd") != 0) {
        return;
    }
    pthread_t t;
    if (pthread_create(&t, NULL, test_thread, NULL) == 0) {
        pthread_detach(t);
    }
}
