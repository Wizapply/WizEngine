#include "Log.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <mutex>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace wizengine {
namespace logging {

namespace {

std::atomic<int> g_minLevel{int(Level::Info)};
bool g_color = false;  // written once in init(), before any threads start
std::mutex g_mutex;

// The name is thread_local, so setThreadName() needs no locking and lookups
// are free. 15 chars + NUL is plenty for "physics"-style names.
thread_local char t_name[16] = {0};

const char* levelName(Level l) {
    switch (l) {
        case Level::Debug: return "DEBUG";
        case Level::Info:  return "INFO ";
        case Level::Warn:  return "WARN ";
        case Level::Error: return "ERROR";
    }
    return "?????";
}

const char* levelColor(Level l) {
    switch (l) {
        case Level::Debug: return "\x1b[90m";    // dim gray
        case Level::Info:  return "\x1b[36m";    // cyan
        case Level::Warn:  return "\x1b[33m";    // yellow
        case Level::Error: return "\x1b[31;1m";  // bright red
    }
    return "";
}

const char* threadName() {
    if (t_name[0]) return t_name;
    // A thread that never called setThreadName: derive a stable short id once
    // so its lines can still be told apart.
    static thread_local char fallback[16] = {0};
    if (!fallback[0]) {
        const std::size_t h =
            std::hash<std::thread::id>{}(std::this_thread::get_id());
        std::snprintf(fallback, sizeof(fallback), "t-%04x",
                      unsigned(h) & 0xffffu);
    }
    return fallback;
}

}  // namespace

void init() {
    // Colour only when stdout really is a console - redirected output (a file,
    // a pipe) gets plain text.
#ifdef _WIN32
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (out != INVALID_HANDLE_VALUE && GetConsoleMode(out, &mode)) {
        g_color =
            SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
    }
#else
    g_color = isatty(fileno(stdout)) != 0;
#endif

    if (const char* env = std::getenv("WIZENGINE_LOG")) {
        if (std::strcmp(env, "debug") == 0) setLevel(Level::Debug);
        else if (std::strcmp(env, "info") == 0) setLevel(Level::Info);
        else if (std::strcmp(env, "warn") == 0) setLevel(Level::Warn);
        else if (std::strcmp(env, "error") == 0) setLevel(Level::Error);
    }
}

void setLevel(Level minLevel) { g_minLevel.store(int(minLevel)); }

void setThreadName(const char* name) {
    std::snprintf(t_name, sizeof(t_name), "%s", name ? name : "");
}

void write(Level level, const char* tag, const char* fmt, ...) {
    if (int(level) < g_minLevel.load(std::memory_order_relaxed)) return;

    char msg[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    // Trim trailing newlines: the prefix-per-line format supplies its own, and
    // this keeps call sites converted from printf (which ended in "\n") clean.
    std::size_t n = std::strlen(msg);
    while (n > 0 && (msg[n - 1] == '\n' || msg[n - 1] == '\r')) msg[--n] = '\0';

    // Wall-clock time with milliseconds.
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    const int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(
                           now.time_since_epoch())
                           .count() %
                       1000);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif

    // One fprintf per line under the lock: lines from different threads never
    // interleave. Flushed so a crash never eats the lines before it.
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_color) {
        std::fprintf(stdout,
                     "\x1b[90m%02d:%02d:%02d.%03d\x1b[0m %s%s\x1b[0m "
                     "\x1b[35m%-8s\x1b[0m %s%s%s%s\n",
                     tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms, levelColor(level),
                     levelName(level), threadName(),
                     (tag && tag[0]) ? "\x1b[90m" : "",
                     (tag && tag[0]) ? tag : "",
                     (tag && tag[0]) ? ":\x1b[0m " : "", msg);
    } else {
        std::fprintf(stdout, "%02d:%02d:%02d.%03d %s %-8s %s%s%s\n",
                     tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms, levelName(level),
                     threadName(), (tag && tag[0]) ? tag : "",
                     (tag && tag[0]) ? ": " : "", msg);
    }
    std::fflush(stdout);
}

}  // namespace logging
}  // namespace wizengine
