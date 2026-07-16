#include "monitoring/drive_detector.hpp"
#include "logger.hpp"
#include <algorithm>
#include <chrono>
#include <thread>
#include <set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#endif

DriveDetector::DriveDetector() = default;
DriveDetector::~DriveDetector() { stop(); }

#ifdef _WIN32
static bool is_ssd(const std::string& mount_point) {
    std::string vol = "\\\\.\\" + mount_point.substr(0, 2);
    // Remove trailing backslash
    if (!vol.empty() && vol.back() == '\\')
        vol.pop_back();

    HANDLE h = CreateFileA(vol.c_str(), 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    STORAGE_PROPERTY_QUERY query;
    memset(&query, 0, sizeof(query));
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType = PropertyStandardQuery;

    DEVICE_SEEK_PENALTY_DESCRIPTOR penalty;
    DWORD bytes = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
                              &query, sizeof(query),
                              &penalty, sizeof(penalty),
                              &bytes, nullptr);
    CloseHandle(h);

    if (!ok) return false;
    // No seek penalty = SSD
    return penalty.IncursSeekPenalty == FALSE;
}
#endif

DriveHardwareType DriveDetector::classify_drive(const std::string& mount_point) {
#ifdef _WIN32
    UINT type = GetDriveTypeA(mount_point.c_str());
    switch (type) {
        case DRIVE_REMOVABLE:
            return DriveHardwareType::USB;
        case DRIVE_FIXED:
            return is_ssd(mount_point) ? DriveHardwareType::SSD : DriveHardwareType::HDD;
        case DRIVE_REMOTE:
            return DriveHardwareType::REMOTE;
        case DRIVE_CDROM:
            return DriveHardwareType::UNKNOWN;
        default:
            return DriveHardwareType::UNKNOWN;
    }
#else
    (void)mount_point;
    return DriveHardwareType::UNKNOWN;
#endif
}

std::vector<DriveInfo> DriveDetector::enumerate_drives() {
    std::vector<DriveInfo> result;

#ifdef _WIN32
    DWORD mask = GetLogicalDrives();
    char root[4] = "A:\\";

    for (int i = 0; i < 26; ++i) {
        if (mask & (1 << i)) {
            root[0] = 'A' + i;
            UINT type = GetDriveTypeA(root);
            if (type == DRIVE_NO_ROOT_DIR || type == DRIVE_CDROM)
                continue;

            DriveInfo di;
            di.mount_point = std::string(1, 'A' + i) + ":\\";
            di.hardware_type = classify_drive(di.mount_point);

            // Get volume label
            char label[256] = {0};
            DWORD vsn = 0, maxlen = 256, flags = 0;
            if (GetVolumeInformationA(root, label, sizeof(label),
                                      &vsn, &maxlen, &flags, nullptr, 0))
                di.label = label;
            if (di.label.empty())
                di.label = "(" + di.mount_point.substr(0, 2) + ")";

            // Get free/total space
            ULARGE_INTEGER free_bytes, total_bytes;
            if (GetDiskFreeSpaceExA(root, &free_bytes, &total_bytes, nullptr)) {
                di.free_size_bytes = free_bytes.QuadPart;
                di.total_size_bytes = total_bytes.QuadPart;
            }

            result.push_back(di);
        }
    }
#endif

    return result;
}

void DriveDetector::poll_loop() {
    LOG_INFO("DriveDetector", "Polling for new drives every " +
             std::to_string(poll_interval_sec_) + "s");

    // Populate known drives
    auto drives = enumerate_drives();
    for (const auto& d : drives)
        known_drives_.push_back(d.mount_point);

    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(poll_interval_sec_));
        if (!running_.load()) break;

        auto current = enumerate_drives();
        std::set<std::string> current_set;
        for (const auto& d : current)
            current_set.insert(d.mount_point);

        std::set<std::string> known_set(known_drives_.begin(), known_drives_.end());

        // Detect added drives
        for (const auto& d : current) {
            if (known_set.find(d.mount_point) == known_set.end()) {
                LOG_INFO("DriveDetector", "New drive detected: " +
                         d.mount_point + " (" + d.label + ") type=" +
                         std::to_string(static_cast<int>(d.hardware_type)));
                if (callback_)
                    callback_(d, true);
                known_drives_.push_back(d.mount_point);
            }
        }

        // Detect removed drives
        for (const auto& mp : known_drives_) {
            if (current_set.find(mp) == current_set.end()) {
                LOG_INFO("DriveDetector", "Drive removed: " + mp);
                // Drive was unplugged - keep in catalog but log removal
                if (callback_) {
                    DriveInfo removed;
                    removed.mount_point = mp;
                    callback_(removed, false);
                }
            }
        }

        known_drives_.clear();
        for (const auto& d : current)
            known_drives_.push_back(d.mount_point);
    }
}

bool DriveDetector::start(DriveCallback callback) {
    if (running_.load()) return false;
    callback_ = std::move(callback);
    running_.store(true);
    worker_ = std::thread([this]() { poll_loop(); });
    worker_.detach();
    return true;
}

void DriveDetector::stop() {
    running_.store(false);
    if (worker_.joinable())
        worker_.join();
}

bool DriveDetector::is_running() const {
    return running_.load();
}
