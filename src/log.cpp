#include "oal/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace oal {
namespace {

std::atomic<int> g_level{static_cast<int>(LogLevel::Info)};
std::atomic<bool> g_color{true};
std::mutex g_mutex;
std::FILE* g_file = nullptr;

const char* color_for(LogLevel level) {
    switch (level) {
        case LogLevel::Trace:
            return "\033[90m";
        case LogLevel::Debug:
            return "\033[36m";
        case LogLevel::Info:
            return "\033[32m";
        case LogLevel::Warn:
            return "\033[33m";
        case LogLevel::Error:
            return "\033[31m";
        case LogLevel::Off:
            break;
    }
    return "";
}

void timestamp(char* buffer, std::size_t size) {
    const auto now = std::chrono::system_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) %
                    1000;
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::snprintf(buffer, size, "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<int>(ms.count()));
}

}  // namespace

void set_log_level(LogLevel level) { g_level.store(static_cast<int>(level)); }

LogLevel log_level() { return static_cast<LogLevel>(g_level.load()); }

void set_log_use_color(bool enabled) { g_color.store(enabled); }

bool set_log_file(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file != nullptr) {
        std::fclose(g_file);
        g_file = nullptr;
    }
    if (path.empty()) {
        return true;
    }
    g_file = std::fopen(path.c_str(), "a");
    return g_file != nullptr;
}

bool should_log(LogLevel level) {
    return static_cast<int>(level) >= g_level.load();
}

const char* level_name(LogLevel level) {
    switch (level) {
        case LogLevel::Trace:
            return "TRACE";
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Info:
            return "INFO";
        case LogLevel::Warn:
            return "WARN";
        case LogLevel::Error:
            return "ERROR";
        case LogLevel::Off:
            return "OFF";
    }
    return "?";
}

void log_write(LogLevel level, const std::string& tag, const std::string& message) {
    if (!should_log(level)) {
        return;
    }
    char ts[32];
    timestamp(ts, sizeof(ts));

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file != nullptr) {
        std::fprintf(g_file, "%s %-5s [%s] %s\n", ts, level_name(level), tag.c_str(),
                     message.c_str());
        std::fflush(g_file);
        return;
    }
    if (g_color.load()) {
        std::fprintf(stderr, "%s%s %-5s %s[%s]%s %s\n", color_for(level), ts, level_name(level),
                     color_for(level), tag.c_str(), "\033[0m", message.c_str());
    } else {
        std::fprintf(stderr, "%s %-5s [%s] %s\n", ts, level_name(level), tag.c_str(),
                     message.c_str());
    }
    std::fflush(stderr);
}

}  // namespace oal
