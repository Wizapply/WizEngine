#pragma once

// Console logging for the whole engine. One line per event, with wall-clock
// time (milliseconds), level, the calling thread's name and a short tag:
//
//   22:38:00.123 INFO  main     hdr: 4096x2048 downsampled to 2048x1024 (1/2)
//   22:38:01.456 WARN  render   grab line: line.filamat not found
//
// Call logging::init() first thing in main(), then use the LOG* macros
// anywhere. printf-style formatting; a trailing "\n" in the format string is
// tolerated (trimmed), so converted printf call sites keep working as-is.
//
// The minimum level defaults to Info and can be overridden with the
// WIZENGINE_LOG environment variable (debug | info | warn | error).
//
// Thread-safe: lines from different threads never interleave. Every line is
// flushed, so the log is complete up to the instant of a crash - worth the
// negligible cost at this log volume.

namespace wizengine {
namespace logging {

enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3 };

// Enables colour when stdout is a real console (on Windows this switches the
// console to VT mode) and reads WIZENGINE_LOG. Safe to call more than once.
void init();

void setLevel(Level minLevel);

// Names the calling thread in every subsequent line ("main", "render",
// "physics", "input", ...). Threads that never call this show a short id.
void setThreadName(const char* name);

// printf-style. Prefer the macros below.
void write(Level level, const char* tag, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

}  // namespace logging
}  // namespace wizengine

// Call sites: LOGI("ibl", "%s (%dx%d)", name, w, h);
// The tag is a short category ("hdr", "cpu", "physics", ...), not the thread -
// the thread name is added automatically.
#define LOGD(tag, ...) \
    ::wizengine::logging::write(::wizengine::logging::Level::Debug, tag, __VA_ARGS__)
#define LOGI(tag, ...) \
    ::wizengine::logging::write(::wizengine::logging::Level::Info, tag, __VA_ARGS__)
#define LOGW(tag, ...) \
    ::wizengine::logging::write(::wizengine::logging::Level::Warn, tag, __VA_ARGS__)
#define LOGE(tag, ...) \
    ::wizengine::logging::write(::wizengine::logging::Level::Error, tag, __VA_ARGS__)
