//
// ProcMapsHook.cpp
// Hooks fopen/fopen64 to filter /proc/self/maps content.
// Replaces bbox sandbox paths with clean virtual paths so guest apps
// (games, anti-cheat SDKs) cannot detect they are running inside the
// virtual container.
//
// Replacements performed on each line:
//   <host_data_dir>/data/user/<uid>/<pkg>/  ->  /data/data/<pkg>/
//   <host_data_dir>/data/user_de/<uid>/<pkg>/  ->  /data/user_de/<uid>/<pkg>/
//   <host_data_dir>/data/app/<pkg>/  ->  /data/app/<pkg>/
//   SdCard/Android/  ->  Android/
//

#include "ProcMapsHook.h"
#include "Dobby/dobby.h"
#include "../Log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

static char g_host_data_dir[512] = {};   // e.g. /data/data/com.mundo.bbox/files

// ---------------------------------------------------------------------------
// String utilities
// ---------------------------------------------------------------------------

// Replace first occurrence of `from` in `src` with `to`.
// Returns a malloc'd string on match, nullptr on no match.
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

// Extract the package name from a path component that starts at `start`.
// The package name ends at the next '/'.  Writes into `out` (max out_size).
// Returns false if not found or too long.
static bool extract_pkg(const char *start, char *out, size_t out_size) {
    const char *end = strchr(start, '/');
    if (!end) end = start + strlen(start);  // end of string
    size_t len = (size_t)(end - start);
    if (len == 0 || len >= out_size) return false;
    memcpy(out, start, len);
    out[len] = '\0';
    // Sanity: package names contain '.', not spaces
    if (!strchr(out, '.')) return false;
    return true;
}

// Apply all bbox-path replacements to `line` (NUL-terminated).
// Result is written into `out` (size out_size).
static void clean_maps_line(const char *line, char *out, size_t out_size) {
    // Work in a local heap buffer so we can chain replacements.
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
                // skip uid digits
                const char *uid_end = strchr(after_pfx, '/');
                if (uid_end) {
                    const char *pkg_start = uid_end + 1;
                    char guest_pkg[256];
                    if (extract_pkg(pkg_start, guest_pkg, sizeof(guest_pkg))) {
                        // Build full target and replacement
                        char target[700], repl[300];
                        snprintf(target, sizeof(target), "%s/data/user/%.*s/%s/",
                                 g_host_data_dir,
                                 (int)(pkg_start - after_pfx),
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
                const char *uid_end = strchr(after_pfx, '/');
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

    // ---- SdCard/Android/ -> Android/ ----
    if (strstr(cur, "SdCard/Android/")) {
        char *tmp = str_replace_first(cur, "SdCard/Android/", "Android/");
        if (tmp) { free(cur); cur = tmp; }
    }

    strncpy(out, cur, out_size - 1);
    out[out_size - 1] = '\0';
    free(cur);
}

// ---------------------------------------------------------------------------
// Filtered maps FILE* builder
// ---------------------------------------------------------------------------

static FILE *build_filtered_maps() {
    FILE *real = fopen("/proc/self/maps", "r");
    if (!real) return nullptr;

    char *content = nullptr;
    size_t content_sz = 0;
    FILE *ms = open_memstream(&content, &content_sz);
    if (!ms) { fclose(real); return nullptr; }

    char line[2048];
    char filtered[2048];
    while (fgets(line, sizeof(line), real)) {
        clean_maps_line(line, filtered, sizeof(filtered));
        fputs(filtered, ms);
    }
    fclose(real);
    fclose(ms);  // flushes; content/content_sz are now valid

    if (!content || content_sz == 0) {
        free(content);
        return nullptr;
    }

    // fmemopen uses the buffer in-place ("r" mode, no copy on Android bionic).
    // content is intentionally not freed here — it backs the returned FILE*.
    // The leak is bounded: one maps snapshot (~50-200 KB), created at most a
    // handful of times during app startup.
    FILE *result = fmemopen(content, content_sz, "r");
    if (!result) free(content);
    return result;
}

// ---------------------------------------------------------------------------
// Is this path /proc/self/maps or /proc/<pid>/maps?
// ---------------------------------------------------------------------------

static bool is_maps_path(const char *path) {
    if (!path) return false;
    if (strcmp(path, "/proc/self/maps") == 0) return true;
    if (strncmp(path, "/proc/", 6) != 0) return false;
    // /proc/<digits>/maps
    const char *p = path + 6;
    while (*p >= '0' && *p <= '9') p++;
    return strcmp(p, "/maps") == 0;
}

// ---------------------------------------------------------------------------
// Hooked fopen / fopen64
// ---------------------------------------------------------------------------

static FILE *(*orig_fopen)(const char *, const char *) = nullptr;
static FILE *hooked_fopen(const char *path, const char *mode) {
    if (is_maps_path(path)) {
        ALOGD("ProcMapsHook: fopen(%s) intercepted", path);
        FILE *f = build_filtered_maps();
        if (f) return f;
    }
    return orig_fopen(path, mode);
}

static FILE *(*orig_fopen64)(const char *, const char *) = nullptr;
static FILE *hooked_fopen64(const char *path, const char *mode) {
    if (is_maps_path(path)) {
        FILE *f = build_filtered_maps();
        if (f) return f;
    }
    return orig_fopen64(path, mode);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void ProcMapsHook::setHostDataDir(const char *hostDataDir) {
    if (hostDataDir && hostDataDir[0]) {
        snprintf(g_host_data_dir, sizeof(g_host_data_dir), "%s", hostDataDir);
        // Strip trailing slash if present
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

    if (fopen_sym) {
        DobbyHook(fopen_sym, (void *)hooked_fopen, (void **)&orig_fopen);
        ALOGD("ProcMapsHook: hooked fopen");
    }
    if (fopen64_sym && fopen64_sym != fopen_sym) {
        DobbyHook(fopen64_sym, (void *)hooked_fopen64, (void **)&orig_fopen64);
        ALOGD("ProcMapsHook: hooked fopen64");
    }

    dlclose(libc);
}
