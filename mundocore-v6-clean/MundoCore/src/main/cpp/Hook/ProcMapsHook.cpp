//
// ProcMapsHook.cpp
// Hooks fopen/fopen64 AND open/openat to filter /proc/self/maps content.
// Covers both stdio-layer readers (most JVM/Unity code) and direct fd-layer
// readers (TSS, GameGuard, and other native anti-cheat SDKs).
//
// Replacements performed on each maps line:
//   <host_data_dir>/data/user/<uid>/<pkg>/  ->  /data/data/<pkg>/
//   <host_data_dir>/data/user_de/<uid>/<pkg>/  ->  /data/user_de/<uid>/<pkg>/
//   <host_data_dir>/data/app/<pkg>/  ->  /data/app/<pkg>/
//   <host_data_dir>/SdCard/  ->  /storage/emulated/0/
//

#include "ProcMapsHook.h"
#include "Dobby/dobby.h"
#include "../Log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/syscall.h>

static char g_host_data_dir[512] = {};

// Per-thread re-entrance guard. Prevents the hook from triggering itself when
// build_filtered_content() calls orig_open/orig_fopen internally.
static thread_local bool g_in_maps_build = false;

// ---------------------------------------------------------------------------
// String utilities
// ---------------------------------------------------------------------------

static char *str_replace_first(const char *src, const char *from, const char *to) {
    const char *pos = strstr(src, from);
    if (!pos) return nullptr;

    size_t from_len = strlen(from);
    size_t to_len   = strlen(to);
    size_t src_len  = strlen(src);
    size_t pre_len  = (size_t)(pos - src);

    char *result = (char *)malloc(src_len - from_len + to_len + 1);
    if (!result) return nullptr;

    memcpy(result, src, pre_len);
    memcpy(result + pre_len, to, to_len);
    strcpy(result + pre_len + to_len, pos + from_len);
    return result;
}

// ---------------------------------------------------------------------------
// Per-line path cleaning
// ---------------------------------------------------------------------------

static bool extract_pkg(const char *start, char *out, size_t out_size) {
    const char *end = strchr(start, '/');
    if (!end) end = start + strlen(start);
    size_t len = (size_t)(end - start);
    if (len == 0 || len >= out_size) return false;
    memcpy(out, start, len);
    out[len] = '\0';
    if (!strchr(out, '.')) return false;
    return true;
}

static void clean_maps_line(const char *line, char *out, size_t out_size) {
    char *cur = strdup(line);
    if (!cur) {
        strncpy(out, line, out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }

    if (g_host_data_dir[0]) {
        // ---- data/user/<uid>/<pkg>/ ----
        {
            char pfx[600];
            snprintf(pfx, sizeof(pfx), "%s/data/user/", g_host_data_dir);
            const char *pos = strstr(cur, pfx);
            if (pos) {
                const char *after_pfx = pos + strlen(pfx);
                const char *uid_end   = strchr(after_pfx, '/');
                if (uid_end) {
                    const char *pkg_start = uid_end + 1;
                    char guest_pkg[256];
                    if (extract_pkg(pkg_start, guest_pkg, sizeof(guest_pkg))) {
                        char target[700], repl[300];
                        // (pkg_start - after_pfx) includes trailing '/' of the uid
                        // segment; subtract 1 so %.*s prints just the digits ("0"),
                        // then the format string's own '/' gives a single separator.
                        snprintf(target, sizeof(target), "%s/data/user/%.*s/%s/",
                                 g_host_data_dir,
                                 (int)(pkg_start - after_pfx - 1),
                                 after_pfx,
                                 guest_pkg);
                        snprintf(repl, sizeof(repl), "/data/data/%s/", guest_pkg);
                        char *tmp = str_replace_first(cur, target, repl);
                        if (tmp) { free(cur); cur = tmp; }
                    }
                }
            }
        }

        // ---- data/user_de/<uid>/<pkg>/ ----
        {
            char pfx[600];
            snprintf(pfx, sizeof(pfx), "%s/data/user_de/", g_host_data_dir);
            const char *pos = strstr(cur, pfx);
            if (pos) {
                const char *after_pfx = pos + strlen(pfx);
                const char *uid_end   = strchr(after_pfx, '/');
                if (uid_end) {
                    const char *pkg_start = uid_end + 1;
                    char uid_buf[32];
                    size_t uid_len = (size_t)(pkg_start - 1 - after_pfx);
                    if (uid_len < sizeof(uid_buf)) {
                        memcpy(uid_buf, after_pfx, uid_len);
                        uid_buf[uid_len] = '\0';
                        char guest_pkg[256];
                        if (extract_pkg(pkg_start, guest_pkg, sizeof(guest_pkg))) {
                            char target[700], repl[300];
                            snprintf(target, sizeof(target),
                                     "%s/data/user_de/%s/%s/",
                                     g_host_data_dir, uid_buf, guest_pkg);
                            snprintf(repl, sizeof(repl),
                                     "/data/user_de/%s/%s/", uid_buf, guest_pkg);
                            char *tmp = str_replace_first(cur, target, repl);
                            if (tmp) { free(cur); cur = tmp; }
                        }
                    }
                }
            }
        }

        // ---- data/app/<pkg>/ ----
        {
            char pfx[600];
            snprintf(pfx, sizeof(pfx), "%s/data/app/", g_host_data_dir);
            const char *pos = strstr(cur, pfx);
            if (pos) {
                const char *pkg_start = pos + strlen(pfx);
                char guest_pkg[256];
                if (extract_pkg(pkg_start, guest_pkg, sizeof(guest_pkg))) {
                    char target[700], repl[300];
                    snprintf(target, sizeof(target),
                             "%s/data/app/%s/", g_host_data_dir, guest_pkg);
                    snprintf(repl, sizeof(repl), "/data/app/%s/", guest_pkg);
                    char *tmp = str_replace_first(cur, target, repl);
                    if (tmp) { free(cur); cur = tmp; }
                }
            }
        }
    }

    // ---- <host_data_dir>/SdCard/ -> /storage/emulated/0/ ----
    // This fully cleans the bbox external storage path so the game sees the
    // expected real sdcard path instead of the sandbox host prefix.
    // e.g. /data/data/com.mundo.bbox/files/SdCard/Android/obb/com.pubg.imobile/
    //   -> /storage/emulated/0/Android/obb/com.pubg.imobile/
    if (g_host_data_dir[0]) {
        char sdcard_pfx[600];
        snprintf(sdcard_pfx, sizeof(sdcard_pfx), "%s/SdCard/", g_host_data_dir);
        if (strstr(cur, sdcard_pfx)) {
            char *tmp = str_replace_first(cur, sdcard_pfx, "/storage/emulated/0/");
            if (tmp) { free(cur); cur = tmp; }
        }
    }
    // Fallback: bare SdCard/Android/ pattern (e.g. paths without the full prefix)
    if (strstr(cur, "SdCard/Android/")) {
        char *tmp = str_replace_first(cur, "SdCard/Android/", "Android/");
        if (tmp) { free(cur); cur = tmp; }
    }

    strncpy(out, cur, out_size - 1);
    out[out_size - 1] = '\0';
    free(cur);
}

// ---------------------------------------------------------------------------
// Hook pointers — declared before build_filtered_content() references them
// ---------------------------------------------------------------------------

static FILE *(*orig_fopen)(const char *, const char *)   = nullptr;
static FILE *(*orig_fopen64)(const char *, const char *) = nullptr;

typedef int (*open_fn_t)(const char *, int, ...);
typedef int (*openat_fn_t)(int, const char *, int, ...);
static open_fn_t   orig_open   = nullptr;
static openat_fn_t orig_openat = nullptr;

// ---------------------------------------------------------------------------
// Core: read and filter /proc/self/maps into a malloc'd buffer.
// MUST only be called with g_in_maps_build == true (caller sets the flag).
// Returns malloc'd buffer (caller must free) and sets *out_sz, or nullptr.
// ---------------------------------------------------------------------------

static char *build_filtered_content(size_t *out_sz) {
    FILE *real = nullptr;

    // Prefer orig_open — bypasses stdio wrapper entirely.
    if (orig_open) {
        int fd = orig_open("/proc/self/maps", O_RDONLY);
        if (fd >= 0) {
            real = fdopen(fd, "r");
            if (!real) close(fd);
        }
    }
    // When open() == openat() on this platform, orig_open stays nullptr;
    // fall back to openat(AT_FDCWD, ...).
    if (!real && orig_openat) {
        int fd = orig_openat(AT_FDCWD, "/proc/self/maps", O_RDONLY);
        if (fd >= 0) {
            real = fdopen(fd, "r");
            if (!real) close(fd);
        }
    }
    // Final fallback: orig_fopen (always set before any hook can fire).
    if (!real && orig_fopen) {
        real = orig_fopen("/proc/self/maps", "r");
    }
    if (!real) return nullptr;

    // Manual growing buffer — avoids open_memstream (requires API 23+)
    size_t cap = 65536;
    char *content = (char *)malloc(cap);
    if (!content) { fclose(real); return nullptr; }
    size_t csz = 0;

    char line[2048], filtered[2048];
    while (fgets(line, sizeof(line), real)) {
        clean_maps_line(line, filtered, sizeof(filtered));
        size_t flen = strlen(filtered);
        if (csz + flen + 1 > cap) {
            cap = (csz + flen + 1) * 2;
            char *tmp = (char *)realloc(content, cap);
            if (!tmp) { free(content); fclose(real); return nullptr; }
            content = tmp;
        }
        memcpy(content + csz, filtered, flen);
        csz += flen;
    }
    fclose(real);

    if (csz == 0) { free(content); return nullptr; }
    content[csz] = '\0';
    *out_sz = csz;
    return content;
}

// ---------------------------------------------------------------------------
// FILE* version — returned by hooked fopen / fopen64
// ---------------------------------------------------------------------------

static FILE *build_filtered_maps() {
    if (g_in_maps_build) return nullptr;
    g_in_maps_build = true;
    size_t csz = 0;
    char *content = build_filtered_content(&csz);
    g_in_maps_build = false;
    if (!content) return nullptr;
    // Create seekable fd, wrap as FILE* — avoids fmemopen (requires API 23+)
    int mfd = -1;
#ifdef __NR_memfd_create
    mfd = (int)syscall(__NR_memfd_create, "maps", 0);
#endif
    if (mfd >= 0) {
        size_t written = 0;
        while (written < csz) {
            ssize_t n = write(mfd, content + written, csz - written);
            if (n <= 0) break;
            written += (size_t)n;
        }
        free(content);
        lseek(mfd, 0, SEEK_SET);
        FILE *f = fdopen(mfd, "r");
        if (!f) close(mfd);
        return f;
    }
    // Pipe fallback (Android 5.x / no memfd_create)
    int pfd[2];
    if (pipe(pfd) != 0) { free(content); return nullptr; }
#ifdef F_SETPIPE_SZ
    fcntl(pfd[1], F_SETPIPE_SZ, (int)(csz + 4096));
#endif
    size_t pw = 0;
    while (pw < csz) {
        ssize_t n = write(pfd[1], content + pw, csz - pw);
        if (n <= 0) break;
        pw += (size_t)n;
    }
    close(pfd[1]);
    free(content);
    FILE *f = fdopen(pfd[0], "r");
    if (!f) close(pfd[0]);
    return f;
}

// ---------------------------------------------------------------------------
// fd version — returned by hooked open / openat
// ---------------------------------------------------------------------------

static int build_filtered_maps_fd() {
    if (g_in_maps_build) return -1;
    g_in_maps_build = true;
    size_t csz = 0;
    char *content = build_filtered_content(&csz);
    g_in_maps_build = false;
    if (!content) return -1;

    // Prefer memfd_create: seekable, no size limit, no pipe-deadlock risk.
    // Available on Android 6+ (kernel 3.17+, __NR_memfd_create defined in NDK).
    int mfd = -1;
#ifdef __NR_memfd_create
    mfd = (int)syscall(__NR_memfd_create, "maps", 0);
#endif
    if (mfd >= 0) {
        size_t written = 0;
        while (written < csz) {
            ssize_t n = write(mfd, content + written, csz - written);
            if (n <= 0) break;
            written += (size_t)n;
        }
        free(content);
        lseek(mfd, 0, SEEK_SET);
        return mfd;
    }

    // Fallback for Android 5.x: pipe. Resize the kernel pipe buffer to fit
    // the content so we can write everything before the caller reads (avoids
    // a blocking-write deadlock on large maps files).
    int pfd[2];
    if (pipe(pfd) != 0) { free(content); return -1; }
#ifdef F_SETPIPE_SZ
    fcntl(pfd[1], F_SETPIPE_SZ, (int)(csz + 4096));
#endif
    size_t written = 0;
    while (written < csz) {
        ssize_t n = write(pfd[1], content + written, csz - written);
        if (n <= 0) break;
        written += (size_t)n;
    }
    close(pfd[1]);
    free(content);
    return pfd[0];
}

// ---------------------------------------------------------------------------
// Is this path /proc/self/maps or /proc/<pid>/maps?
// ---------------------------------------------------------------------------

static bool is_maps_path(const char *path) {
    if (!path) return false;
    if (strcmp(path, "/proc/self/maps") == 0) return true;
    if (strncmp(path, "/proc/", 6) != 0) return false;
    const char *p = path + 6;
    while (*p >= '0' && *p <= '9') p++;
    return strcmp(p, "/maps") == 0;
}

// ---------------------------------------------------------------------------
// Hooked fopen / fopen64
// ---------------------------------------------------------------------------

static FILE *hooked_fopen(const char *path, const char *mode) {
    if (is_maps_path(path)) {
        ALOGD("ProcMapsHook: fopen(%s) intercepted", path);
        FILE *f = build_filtered_maps();
        if (f) return f;
    }
    return orig_fopen(path, mode);
}

static FILE *hooked_fopen64(const char *path, const char *mode) {
    if (is_maps_path(path)) {
        FILE *f = build_filtered_maps();
        if (f) return f;
    }
    return orig_fopen64(path, mode);
}

// ---------------------------------------------------------------------------
// Hooked open / openat — covers direct fd-layer readers (TSS, GameGuard…)
// ---------------------------------------------------------------------------

static int hooked_open(const char *path, int flags, ...) {
    if (is_maps_path(path)) {
        ALOGD("ProcMapsHook: open(%s) intercepted", path);
        int fd = build_filtered_maps_fd();
        if (fd >= 0) return fd;
    }
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
    }
    return orig_open(path, flags, mode);
}

static int hooked_openat(int dirfd, const char *path, int flags, ...) {
    if (is_maps_path(path)) {
        ALOGD("ProcMapsHook: openat(%s) intercepted", path);
        int fd = build_filtered_maps_fd();
        if (fd >= 0) return fd;
    }
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
    }
    return orig_openat(dirfd, path, flags, mode);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void ProcMapsHook::setHostDataDir(const char *hostDataDir) {
    if (hostDataDir && hostDataDir[0]) {
        snprintf(g_host_data_dir, sizeof(g_host_data_dir), "%s", hostDataDir);
        size_t len = strlen(g_host_data_dir);
        if (len > 0 && g_host_data_dir[len - 1] == '/') {
            g_host_data_dir[len - 1] = '\0';
        }
        ALOGD("ProcMapsHook: hostDataDir = %s", g_host_data_dir);
    }
}

void ProcMapsHook::init() {
    void *libc = dlopen("libc.so", RTLD_NOW | RTLD_NOLOAD);
    if (!libc) libc = dlopen("libc.so", RTLD_NOW);
    if (!libc) {
        ALOGD("ProcMapsHook: dlopen libc.so failed");
        return;
    }

    void *fopen_sym   = dlsym(libc, "fopen");
    void *fopen64_sym = dlsym(libc, "fopen64");
    void *open_sym    = dlsym(libc, "open");
    void *openat_sym  = dlsym(libc, "openat");

    if (fopen_sym) {
        DobbyHook(fopen_sym, (void *)hooked_fopen, (void **)&orig_fopen);
        ALOGD("ProcMapsHook: hooked fopen");
    }
    // fopen64 is a distinct symbol only on 32-bit bionic; skip on 64-bit where
    // it aliases fopen (same address), to avoid double-hooking the same function.
    if (fopen64_sym && fopen64_sym != fopen_sym) {
        DobbyHook(fopen64_sym, (void *)hooked_fopen64, (void **)&orig_fopen64);
        ALOGD("ProcMapsHook: hooked fopen64");
    }
    // On arm64 bionic, open() is typically just openat(AT_FDCWD,...) — same
    // function address. Only hook open() separately when it is distinct.
    if (open_sym && open_sym != openat_sym) {
        DobbyHook(open_sym, (void *)hooked_open, (void **)&orig_open);
        ALOGD("ProcMapsHook: hooked open");
    }
    if (openat_sym) {
        DobbyHook(openat_sym, (void *)hooked_openat, (void **)&orig_openat);
        ALOGD("ProcMapsHook: hooked openat");
    }

    dlclose(libc);
}
