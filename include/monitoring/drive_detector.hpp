#pragma once

#include "core/tier.hpp"
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <vector>
#include <cstdint>

enum class DriveHardwareType : int {
    UNKNOWN = 0,
    SSD     = 1,
    HDD     = 2,
    USB     = 3,
    REMOTE  = 4
};

struct DriveInfo {
    std::string       mount_point;
    std::string       label;
    DriveHardwareType hardware_type;
    uint64_t          total_size_bytes;
    uint64_t          free_size_bytes;
};

inline Tier drive_hardware_to_tier(DriveHardwareType dt) {
    switch (dt) {
        case DriveHardwareType::SSD:    return Tier::HOT;
        case DriveHardwareType::HDD:    return Tier::WARM;
        case DriveHardwareType::USB:    return Tier::COLD;
        case DriveHardwareType::REMOTE: return Tier::ARCHIVE;
        default:                        return Tier::WARM;
    }
}

class DriveDetector {
public:
    DriveDetector();
    ~DriveDetector();

    using DriveCallback = std::function<void(const DriveInfo& drive, bool added)>;

    bool start(DriveCallback callback);
    void stop();
    bool is_running() const;

    void set_poll_interval_sec(int sec) { poll_interval_sec_ = sec; }

    static DriveHardwareType classify_drive(const std::string& mount_point);
    static std::vector<DriveInfo> enumerate_drives();

private:
    std::atomic<bool> running_{false};
    std::thread worker_;
    int poll_interval_sec_ = 10;

    std::vector<std::string> known_drives_;
    DriveCallback callback_;

    void poll_loop();
};
