// fcitx5 日志宏的最小兼容层：X11Panel 只用到 FCITX_DECLARE_LOG_CATEGORY / FCITX_LOGC。
//
// 输出到 stderr（fcitx4 前台运行时即会话日志）。Debug 级默认关闭：面板每帧几何、每个按键
// 都打 Debug，生产环境太吵；排查时设 `WIND_LOG_DEBUG=1` 打开。Warn/Info 始终输出。
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace windlinux {

struct LogCategory {
    const char* name;
};

inline bool debugLogEnabled()
{
    static const bool on = std::getenv("WIND_LOG_DEBUG") != nullptr;
    return on;
}

/// 一行日志：析构时统一输出（支持 << 链式）。被关闭的级别不积累内容。
class LogLine {
public:
    LogLine(const char* category, const char* level, bool enabled)
        : category_(category), level_(level), enabled_(enabled)
    {
    }
    ~LogLine()
    {
        if (enabled_) {
            std::fprintf(stderr, "[%s][%s] %s\n", category_, level_, stream_.str().c_str());
        }
    }
    LogLine(const LogLine&) = delete;
    LogLine& operator=(const LogLine&) = delete;

    template <typename T>
    LogLine& operator<<(const T& value)
    {
        if (enabled_) {
            stream_ << value;
        }
        return *this;
    }

private:
    const char* category_;
    const char* level_;
    bool enabled_;
    std::ostringstream stream_;
};

inline LogLine logStream(const LogCategory& category, const char* level)
{
    const bool enabled = std::strcmp(level, "Debug") != 0 || debugLogEnabled();
    return LogLine(category.name, level, enabled);
}

} // namespace windlinux

#define FCITX_DECLARE_LOG_CATEGORY(name) \
    namespace windlinux { \
    extern ::windlinux::LogCategory name; \
    }
#define FCITX_DEFINE_LOG_CATEGORY(name, str) \
    namespace windlinux { \
    ::windlinux::LogCategory name{str}; \
    }
#define FCITX_LOGC(category, level) ::windlinux::logStream(category, #level)
