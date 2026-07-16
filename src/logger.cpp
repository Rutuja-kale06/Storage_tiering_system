#include "logger.hpp"
#include <iostream>
#include <iomanip>

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

Logger::~Logger() {
    if (file_.is_open())
        file_.close();
}

void Logger::init(const std::string& filepath, LogLevel level) {
    level_ = level;

    if (!filepath.empty()) {
        file_.open(filepath, std::ios::app);
        if (file_.is_open()) {
            use_stdout_ = false;
        }
    }
}

void Logger::set_level(LogLevel level) {
    level_ = level;
}

std::string Logger::level_name(LogLevel level) const {
    switch (level) {
        case LogLevel::TRACE: return "TRACE";
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO";
        case LogLevel::WARN:  return "WARN";
        case LogLevel::ERROR: return "ERROR";
        case LogLevel::FATAL: return "FATAL";
    }
    return "UNKNOWN";
}

std::string Logger::timestamp() const {
    time_t now = time(nullptr);
    struct tm tm_info;
#ifdef _WIN32
    localtime_s(&tm_info, &now);
#else
    localtime_r(&now, &tm_info);
#endif
    char buf[24];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_info);
    return std::string(buf);
}

void Logger::log(LogLevel level, const std::string& module,
                 const std::string& message)
{
    if (level < level_) return;

    std::lock_guard<std::mutex> lk(mtx_);
    std::string line = timestamp() + " [" + level_name(level) + "] "
                       + "[" + module + "] " + message;

    if (use_stdout_) {
        if (level >= LogLevel::ERROR)
            std::cerr << line << std::endl;
        else
            std::cout << line << std::endl;
    }

    if (file_.is_open()) {
        file_ << line << "\n";
        // Flush on warnings and above for important messages
        if (level >= LogLevel::WARN)
            file_.flush();
    }
}

void Logger::trace(const std::string& module, const std::string& msg) {
    log(LogLevel::TRACE, module, msg);
}

void Logger::debug(const std::string& module, const std::string& msg) {
    log(LogLevel::DEBUG, module, msg);
}

void Logger::info(const std::string& module, const std::string& msg) {
    log(LogLevel::INFO, module, msg);
}

void Logger::warn(const std::string& module, const std::string& msg) {
    log(LogLevel::WARN, module, msg);
}

void Logger::error(const std::string& module, const std::string& msg) {
    log(LogLevel::ERROR, module, msg);
}

void Logger::fatal(const std::string& module, const std::string& msg) {
    log(LogLevel::FATAL, module, msg);
}
