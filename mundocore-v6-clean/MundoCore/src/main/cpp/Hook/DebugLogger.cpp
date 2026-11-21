#include "DebugLogger.h"
#include <android/log.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <sys/types.h>

#define TRACE_TAG "BBOX_TRACE"

static int             g_fd    = -1;
static char            g_path[512] = {};
static pthread_mutex_t g_mtx   = PTHREAD_MUTEX_INITIALIZER;

void DebugLogger::init(const char *hostDataDir) {
    if (!hostDataDir || !hostDataDir[0]) return;
    pthread_mutex_lock(&g_mtx);
    if (g_fd >= 0) { close(g_fd); g_fd = -1; }
    snprintf(g_path, sizeof(g_path), "%s/bbox_trace.log", hostDataDir);
    g_fd = open(g_path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0666);
    pthread_mutex_unlock(&g_mtx);
    log("=== BBOX TRACE START === pid=%d log=%s", (int)getpid(), g_path);
}

const char *DebugLogger::logPath() { return g_path; }

void DebugLogger::log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char msg[1024];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    __android_log_print(ANDROID_LOG_DEBUG, TRACE_TAG, "%s", msg);

    pthread_mutex_lock(&g_mtx);
    if (g_fd >= 0) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        long ms = ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
        char line[1100];
        int n = snprintf(line, sizeof(line), "[%ld] %s\n", ms, msg);
        if (n > 0 && n < (int)sizeof(line))
            write(g_fd, line, (size_t)n);
    }
    pthread_mutex_unlock(&g_mtx);
}
