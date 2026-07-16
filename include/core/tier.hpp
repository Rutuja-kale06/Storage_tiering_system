#pragma once

#include <string>
#include <array>
#include <cstdint>

enum class Tier : int {
    HOT     = 0,
    WARM    = 1,
    COLD    = 2,
    ARCHIVE = 3
};

constexpr int TIER_COUNT = 4;

constexpr std::array<const char*, TIER_COUNT> TIER_NAMES = {
    "HOT", "WARM", "COLD", "ARCHIVE"
};

constexpr std::array<const char*, TIER_COUNT> TIER_LABELS = {
    "HOT  (NVMe SSD)",
    "WARM (HDD)",
    "COLD (Object)",
    "ARCH (Tape)"
};

constexpr std::array<double, TIER_COUNT> TIER_COST_PER_GB_MONTH = {
    0.20,    // HOT
    0.05,    // WARM
    0.01,    // COLD
    0.002    // ARCHIVE
};

constexpr std::array<double, TIER_COUNT> TIER_LATENCY_MS = {
    0.1,     // HOT
    8.0,     // WARM
    150.0,   // COLD
    4000.0   // ARCHIVE
};

constexpr std::array<int64_t, TIER_COUNT> TIER_IOPS_CAPACITY = {
    40000,   // HOT  - NVMe/Flash
    2000,    // WARM - SAS/HDD
    320,     // COLD - SATA/NL-SAS
    100      // ARCHIVE - Tape/Object
};

constexpr std::array<const char*, TIER_COUNT> TIER_ANSI_COLORS = {
    "\033[91m",  // HOT - red
    "\033[93m",  // WARM - yellow
    "\033[94m",  // COLD - blue
    "\033[95m"   // ARCHIVE - magenta
};

inline std::string tier_name(Tier t) {
    int idx = static_cast<int>(t);
    return (idx >= 0 && idx < TIER_COUNT) ? TIER_NAMES[idx] : "UNKNOWN";
}

inline std::string tier_label(Tier t) {
    int idx = static_cast<int>(t);
    return (idx >= 0 && idx < TIER_COUNT) ? TIER_LABELS[idx] : "UNKNOWN";
}

inline double tier_cost_per_gb(Tier t) {
    int idx = static_cast<int>(t);
    return (idx >= 0 && idx < TIER_COUNT) ? TIER_COST_PER_GB_MONTH[idx] : 0.0;
}

inline double tier_latency_ms(Tier t) {
    int idx = static_cast<int>(t);
    return (idx >= 0 && idx < TIER_COUNT) ? TIER_LATENCY_MS[idx] : 0.0;
}

inline std::string tier_color(Tier t) {
    int idx = static_cast<int>(t);
    return (idx >= 0 && idx < TIER_COUNT) ? TIER_ANSI_COLORS[idx] : "\033[0m";
}

inline int64_t tier_iops_capacity(Tier t) {
    int idx = static_cast<int>(t);
    return (idx >= 0 && idx < TIER_COUNT) ? TIER_IOPS_CAPACITY[idx] : 0;
}
