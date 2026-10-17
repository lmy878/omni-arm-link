#pragma once

#include <cstdint>
#include <sstream>
#include <string>

namespace oal {

enum class LogLevel : int {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Off = 5,
};

void set_log_level(LogLevel level);
LogLevel log_level();
bool set_log_file(const std::string& path);
void set_log_use_color(bool enabled);

bool should_log(LogLevel level);
void log_write(LogLevel level, const std::string& tag, const std::string& message);

const char* level_name(LogLevel level);

class LogStream {
public:
    LogStream(LogLevel level, const char* tag) : level_(level), tag_(tag) {}

    ~LogStream() { log_write(level_, tag_, buffer_.str()); }

    template <typename T>
    LogStream& operator<<(const T& value) {
        buffer_ << value;
        return *this;
    }

private:
    LogLevel level_;
    const char* tag_;
    std::ostringstream buffer_;
};

}  // namespace oal

#define OAL_LOG(level, tag)                     \
    if (!::oal::should_log(level)) {            \
    } else                                      \
        ::oal::LogStream(level, tag)

#define OAL_TRACE(tag) OAL_LOG(::oal::LogLevel::Trace, tag)
#define OAL_DEBUG(tag) OAL_LOG(::oal::LogLevel::Debug, tag)
#define OAL_INFO(tag) OAL_LOG(::oal::LogLevel::Info, tag)
#define OAL_WARN(tag) OAL_LOG(::oal::LogLevel::Warn, tag)
#define OAL_ERROR(tag) OAL_LOG(::oal::LogLevel::Error, tag)
