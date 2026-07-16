#pragma once

#include <string>
#include <fstream>
#include <mutex>
#include <sstream>
#include <ctime>

enum class LogLevel {
    TRACE = 0,
    DEBUG = 1,
    INFO  = 2,
    WARN  = 3,
    ERROR = 4,
    FATAL = 5
};

class Logger {
public:
    static Logger& instance();

    void init(const std::string& filepath = "", LogLevel level = LogLevel::INFO);
    void set_level(LogLevel level);
    LogLevel level() const { return level_; }

    void log(LogLevel level, const std::string& module,
             const std::string& message);
    void trace(const std::string& module, const std::string& msg);
    void debug(const std::string& module, const std::string& msg);
    void info(const std::string& module, const std::string& msg);
    void warn(const std::string& module, const std::string& msg);
    void error(const std::string& module, const std::string& msg);
    void fatal(const std::string& module, const std::string& msg);

    // Structured log with key-value pairs
    template<typename... Args>
    void logkv(LogLevel level, const std::string& module,
               const std::string& message, Args&&... args) {
        std::ostringstream oss;
        oss << message;
        append_kv(oss, std::forward<Args>(args)...);
        log(level, module, oss.str());
    }

private:
    Logger() = default;
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    std::ofstream file_;
    std::mutex    mtx_;
    LogLevel      level_ = LogLevel::INFO;
    bool          use_stdout_ = true;

    std::string level_name(LogLevel level) const;
    std::string timestamp() const;

    template<typename T>
    void append_kv(std::ostringstream& oss, const std::string& key, T&& value) {
        oss << " " << key << "=" << value;
    }

    template<typename T, typename... Args>
    void append_kv(std::ostringstream& oss,
                   const std::string& key, T&& value,
                   Args&&... rest) {
        oss << " " << key << "=" << value;
        append_kv(oss, std::forward<Args>(rest)...);
    }
};

// Convenience macros (disabled in release if desired)
#define LOG_TRACE(module, msg)  ::Logger::instance().trace(module, msg)
#define LOG_DEBUG(module, msg)  ::Logger::instance().debug(module, msg)
#define LOG_INFO(module, msg)   ::Logger::instance().info(module, msg)
#define LOG_WARN(module, msg)   ::Logger::instance().warn(module, msg)
#define LOG_ERROR(module, msg)  ::Logger::instance().error(module, msg)
#define LOG_FATAL(module, msg)  ::Logger::instance().fatal(module, msg)
