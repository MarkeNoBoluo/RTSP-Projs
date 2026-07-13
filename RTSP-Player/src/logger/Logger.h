#pragma once

#include <string>
#include <mutex>
#include <cstdarg>
#include <cstdio>

namespace logger {

enum class Level : int { Debug = 0, Info = 1, Warn = 2, Error = 3 };

class Logger {
public:
    static Logger& instance();

    void setLevel(Level lv) { m_level = lv; }
    Level level() const { return m_level; }

    bool initLogFile(const char* path);
    void closeLogFile();

    void log(Level lv, const char* file, int line, const char* fmt, ...);

private:
    Logger() = default;
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    Level m_level = Level::Debug;
    std::mutex m_mutex;
    FILE* m_logFile = nullptr;
};

inline const char* levelName(Level lv) {
    switch (lv) {
        case Level::Debug: return "DEBUG";
        case Level::Info:  return "INFO";
        case Level::Warn:  return "WARN";
        case Level::Error: return "ERROR";
    }
    return "???";
}

inline const char* basename(const char* path) {
    const char* p = path;
    for (const char* q = path; *q; ++q) {
        if (*q == '\\' || *q == '/') p = q + 1;
    }
    return p;
}

} // namespace logger

#define LOG_DEBUG(fmt, ...) logger::Logger::instance().log(logger::Level::Debug, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  logger::Logger::instance().log(logger::Level::Info,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  logger::Logger::instance().log(logger::Level::Warn,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) logger::Logger::instance().log(logger::Level::Error, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
