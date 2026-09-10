#pragma once

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

namespace WildanDev
{

enum class LogLevel
{
    Info,
    Warn,
    Error
};

class Logger
{
public:
    static Logger& instance()
    {
        static Logger logger;
        return logger;
    }

    void setLevel(LogLevel level) { m_level = level; }

    void log(LogLevel level, const std::string& message)
    {
        if (level < m_level)
            return;
        std::lock_guard<std::mutex> guard(m_mutex);

        const char* tag = "INFO";
        if (level == LogLevel::Warn)
            tag = "WARN";
        else if (level == LogLevel::Error)
            tag = "ERROR";

        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        char timeBuf[32] = {};
        std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
        std::fprintf(stderr, "[%s] [%s] %s\n", timeBuf, tag, message.c_str());
    }

private:
    Logger() = default;

    LogLevel m_level{LogLevel::Info};
    std::mutex m_mutex;
};

inline void logInfo(const std::string& message)
{
    Logger::instance().log(LogLevel::Info, message);
}

inline void logWarn(const std::string& message)
{
    Logger::instance().log(LogLevel::Warn, message);
}

inline void logError(const std::string& message)
{
    Logger::instance().log(LogLevel::Error, message);
}

} // namespace WildanDev
