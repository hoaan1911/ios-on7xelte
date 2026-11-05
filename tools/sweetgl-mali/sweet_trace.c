/*
 * Mesa's job trace into the kernel log. PAN_MESA_DEBUG=trace makes Panfrost decode every job chain it
 * submits (pandecode) and print it on stderr, which in backboardd goes nowhere. This points fd 2 at a pipe
 * and forwards what comes out to the persistent log through sweet_drm_log (kernel log, 160 characters a
 * line, "J7 G3D client: " in front), so a faulting job can be read after the boot.
 *
 * On by default while the GPU path is being brought up; the environment variable SWEET_TRACE=0 turns it
 * off. Only the first SWEET_TRACE_LINES lines are logged (the rest is read and dropped, so the writer never
 * blocks).
 */
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void sweet_drm_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));   /* sweet_drm_mali.c */
void sweet_trace_start(void);

#define SWEET_TRACE_LINES 8000
#define SWEET_TRACE_WIDTH 140

static void *
sweet_trace_thread(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char buf[2048];
    char line[SWEET_TRACE_WIDTH + 1];
    size_t used = 0;
    unsigned lines = 0;
    ssize_t n;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];
            int end = (c == '\n');
            if (!end && c != '\r') {
                line[used++] = (c >= 32 && c < 127) ? c : '.';
            }
            if (end || used == SWEET_TRACE_WIDTH) {
                if (used > 0 && lines < SWEET_TRACE_LINES) {
                    line[used] = '\0';
                    sweet_drm_log("t: %s", line);
                    if (++lines == SWEET_TRACE_LINES) {
                        sweet_drm_log("t: (trace stops here, %d lines)", SWEET_TRACE_LINES);
                    }
                }
                used = 0;
            }
        }
    }
    return NULL;
}

void
sweet_trace_start(void)
{
    static int started;
    const char *off = getenv("SWEET_TRACE");
    int fds[2];
    pthread_t thread;

    if (started || (off && off[0] == '0')) {
        return;
    }
    started = 1;
    if (pipe(fds) != 0) {
        return;
    }
    if (dup2(fds[1], 2) < 0) {
        close(fds[0]);
        close(fds[1]);
        return;
    }
    close(fds[1]);
    if (pthread_create(&thread, NULL, sweet_trace_thread, (void *)(intptr_t)fds[0]) != 0) {
        return;
    }
    pthread_detach(thread);
    /* Read by Mesa when it creates the screen. PANDECODE_DUMP_FILE=stderr keeps pandecode off the disk. */
    setenv("PAN_MESA_DEBUG", "trace", 0);
    setenv("PANDECODE_DUMP_FILE", "stderr", 0);
    sweet_drm_log("sweet_trace: job trace on (SWEET_TRACE=0 turns it off)");
}
