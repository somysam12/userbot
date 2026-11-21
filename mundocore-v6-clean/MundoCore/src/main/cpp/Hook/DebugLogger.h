#pragma once

// BBOX_TRACE — comprehensive guest-process tracer.
// Logs to logcat (tag BBOX_TRACE) AND to {host_data_dir}/bbox_trace.log.
// Use: adb logcat -s BBOX_TRACE
// Or:  adb pull $(adb shell run-as <host_pkg> cat /data/data/<host_pkg>/files/bbox_trace.log.path)

class DebugLogger {
public:
    // Call once from setHostDataDir — initialises log file at {dir}/bbox_trace.log.
    static void init(const char *hostDataDir);
    static void log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
    static const char *logPath();
};

#define TRACE(...) DebugLogger::log(__VA_ARGS__)
