// SPDX-License-Identifier: MIT
// J7 Prime port: where did backboardd crash? ReportCrash's own line (the kernel logs it) gives only
// addresses inside the dyld shared cache, which cannot be read without the cache. From inside the process,
// dladdr() can name the image and the nearest exported symbol, so this logs the faulting pc, the link
// register, the first registers and the frame-pointer chain that way (through sweet_drm_log, so it reaches
// the persistent log), and then lets the crash happen as before.
#ifndef __APPLE__
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>

void sweet_drm_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));   /* sweet_drm_mali.c */
void sweet_crash_report(int sig, uintptr_t pc, uintptr_t lr, uintptr_t fp, uintptr_t sp, uintptr_t far,
                        const uint64_t *x);
void sweet_crash_install(void);

static void
log_addr(const char *tag, uintptr_t a)
{
    Dl_info di;

    if (a != 0 && dladdr((void *)a, &di) != 0 && di.dli_fname != NULL) {
        const char *base = strrchr(di.dli_fname, '/');
        base = base != NULL ? base + 1 : di.dli_fname;
        sweet_drm_log("%s %lx %s+%lx %s+%lx", tag, (unsigned long)a, base,
            (unsigned long)(a - (uintptr_t)di.dli_fbase), di.dli_sname != NULL ? di.dli_sname : "?",
            di.dli_saddr != NULL ? (unsigned long)(a - (uintptr_t)di.dli_saddr) : 0ul);
    } else {
        sweet_drm_log("%s %lx (no image)", tag, (unsigned long)a);
    }
}

void
sweet_crash_report(int sig, uintptr_t pc, uintptr_t lr, uintptr_t fp, uintptr_t sp, uintptr_t far,
                   const uint64_t *x)
{
    sweet_drm_log("crash: sig=%d far=%lx pc=%lx lr=%lx", sig, (unsigned long)far, (unsigned long)pc,
        (unsigned long)lr);
    sweet_drm_log("crash: x0-3 %llx %llx %llx %llx", (unsigned long long)x[0], (unsigned long long)x[1],
        (unsigned long long)x[2], (unsigned long long)x[3]);
    sweet_drm_log("crash: x4-8 %llx %llx %llx %llx %llx", (unsigned long long)x[4], (unsigned long long)x[5],
        (unsigned long long)x[6], (unsigned long long)x[7], (unsigned long long)x[8]);
    log_addr("crash: pc", pc);
    log_addr("crash: lr", lr);
    /* The frame-pointer chain: [fp] is the caller's fp, [fp + 8] the return address. Stay on this stack. */
    uintptr_t *f = (uintptr_t *)fp;
    for (unsigned i = 0; i < 14 && f != NULL && ((uintptr_t)f & 7) == 0 && (uintptr_t)f >= sp &&
         (uintptr_t)f - sp < (1u << 22); i++) {
        char tag[16];
        tag[0] = 'f';
        tag[1] = (char)('0' + i / 10);
        tag[2] = (char)('0' + i % 10);
        tag[3] = '\0';
        log_addr(tag, f[1]);
        uintptr_t *next = (uintptr_t *)f[0];
        if (next <= f) {
            break;
        }
        f = next;
    }
}

#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/ucontext.h>

static void
sweet_crash_handler(int sig, siginfo_t *info, void *context)
{
    ucontext_t *uc = (ucontext_t *)context;
    _STRUCT_ARM_THREAD_STATE64 *ts = &uc->uc_mcontext->__ss;
    uint64_t x[9];

    for (unsigned i = 0; i < 9; i++) {
        x[i] = ts->__x[i];
    }
    sweet_crash_report(sig, (uintptr_t)arm_thread_state64_get_pc(*ts), (uintptr_t)arm_thread_state64_get_lr(*ts),
        (uintptr_t)arm_thread_state64_get_fp(*ts), (uintptr_t)arm_thread_state64_get_sp(*ts),
        (uintptr_t)info->si_addr, x);
    signal(sig, SIG_DFL);       /* the faulting instruction runs again and the crash goes on as before */
}

void
sweet_crash_install(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sweet_crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}
#else
void sweet_crash_install(void) {}
#endif
