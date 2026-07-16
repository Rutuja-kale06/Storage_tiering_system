#pragma once

#include "file_record.hpp"
#include <string>
#include <sstream>
#include <iomanip>
#include <vector>
#include <functional>

inline std::string format_bytes(int64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double val = static_cast<double>(bytes);
    int u = 0;
    while (val >= 1024.0 && u < 4) { val /= 1024.0; ++u; }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << val << " " << units[u];
    return oss.str();
}

inline std::string format_time(time_t t) {
    char buf[32];
    struct tm* tm_info = std::localtime(&t);
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", tm_info);
    return std::string(buf);
}

inline std::string pad_right(const std::string& s, int width) {
    if (static_cast<int>(s.size()) >= width) return s.substr(0, width);
    return s + std::string(width - static_cast<int>(s.size()), ' ');
}

inline std::string pad_left(const std::string& s, int width) {
    if (static_cast<int>(s.size()) >= width) return s.substr(0, width);
    return std::string(width - static_cast<int>(s.size()), ' ') + s;
}

// ANSI color constants
#define RESET   "\033[0m"
#define BOLD    "\033[1m"
#define DIM     "\033[2m"
#define RED     "\033[91m"
#define YELLOW  "\033[93m"
#define BLUE    "\033[94m"
#define CYAN    "\033[96m"
#define GREEN   "\033[92m"
#define MAGENTA "\033[95m"

// Callback types for monitoring
using FileCreatedCallback   = std::function<void(const std::string& path)>;
using FileModifiedCallback  = std::function<void(const std::string& path)>;
using FileAccessedCallback  = std::function<void(const std::string& path)>;
using FileDeletedCallback   = std::function<void(const std::string& path)>;
