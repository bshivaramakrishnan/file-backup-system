#pragma once

#include <cstdio>
#include <ctime>
#include <cstdarg>
#include <mutex>

namespace ecpb {

enum class LogLevel { DEBUG = 0, INFO = 1, WARN = 2, ERR = 3 };

class Logger {
public:
    static Logger& instance() {
        static Logger inst;
        return inst;
    }

    void set_level(LogLevel lvl) { level_ = lvl; }
    LogLevel get_level() const { return level_; }

    void log(LogLevel lvl, const char* fmt, ...) {
        if (lvl < level_) return;
        std::lock_guard<std::mutex> lock(mtx_);
        time_t now = time(nullptr);
        struct tm tm_buf;
        localtime_r(&now, &tm_buf);
        char tbuf[20];
        strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tm_buf);
        const char* tag = "???";
        switch (lvl) {
            case LogLevel::DEBUG: tag = "DBG"; break;
            case LogLevel::INFO:  tag = "INF"; break;
            case LogLevel::WARN:  tag = "WRN"; break;
            case LogLevel::ERR:   tag = "ERR"; break;
        }
        fprintf(stderr, "[%s][%s] ", tbuf, tag);
        va_list args;
        va_start(args, fmt);
        vfprintf(stderr, fmt, args);
        va_end(args);
        fprintf(stderr, "\n");
    }

private:
    Logger() : level_(LogLevel::INFO) {}
    LogLevel level_;
    std::mutex mtx_;
};

#define LOG_DEBUG(fmt, ...) ecpb::Logger::instance().log(ecpb::LogLevel::DEBUG, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  ecpb::Logger::instance().log(ecpb::LogLevel::INFO,  fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  ecpb::Logger::instance().log(ecpb::LogLevel::WARN,  fmt, ##__VA_ARGS__)
#define LOG_ERR(fmt, ...)   ecpb::Logger::instance().log(ecpb::LogLevel::ERR,   fmt, ##__VA_ARGS__)

} // namespace ecpb
