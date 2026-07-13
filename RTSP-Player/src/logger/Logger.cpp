#include "Logger.h"
#include <cstdio>
#include <chrono>
#include <ctime>

#ifdef _WIN32
#include <windows.h>
#endif

namespace logger {

Logger& Logger::instance() {
    static Logger s_instance;
    return s_instance;
}

Logger::~Logger() {
    closeLogFile();
}

bool Logger::initLogFile(const char* path) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_logFile) fclose(m_logFile);

    fopen_s(&m_logFile, path, "a");
    if (!m_logFile) {
        fprintf(stderr, "[LOGGER] Failed to open log file: %s\n", path);
        return false;
    }

    fprintf(m_logFile, "===== Logger initialized =====\n");
    fflush(m_logFile);
    return true;
}

void Logger::closeLogFile() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_logFile) {
        fprintf(m_logFile, "===== Logger closed =====\n");
        fflush(m_logFile);
        fclose(m_logFile);
        m_logFile = nullptr;
    }
}

void Logger::log(Level lv, const char* file, int line, const char* fmt, ...) {
    if (lv < m_level) return;

    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;

    std::tm tm_buf;
    localtime_s(&tm_buf, &t);

    char timeBuf[32];
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d.%03d",
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, (int)ms.count());

    va_list args;
    va_start(args, fmt);
    char msgBuf[1024];
    vsnprintf(msgBuf, sizeof(msgBuf), fmt, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(m_mutex);

    char lineBuf[1536];
    int len = snprintf(lineBuf, sizeof(lineBuf),
                       "[%s] [%s] [%s:%d] [tid:%u] %s\n",
                       timeBuf, levelName(lv), basename(file), line,
                       (unsigned)GetCurrentThreadId(), msgBuf);

    fwrite(lineBuf, 1, len, stderr);
    fflush(stderr);

    if (m_logFile) {
        fwrite(lineBuf, 1, len, m_logFile);
        fflush(m_logFile);
    }
}

} // namespace logger
