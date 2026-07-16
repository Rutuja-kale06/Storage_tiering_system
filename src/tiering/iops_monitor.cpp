#include "tiering/iops_monitor.hpp"
#include "catalog/catalog_interface.hpp"
#include "core/types.hpp"
#include <chrono>
#include <random>
#include <algorithm>
#include <cstdlib>

IopsMonitor::IopsMonitor() = default;

IopsMonitor::~IopsMonitor() {
    stop();
}

bool IopsMonitor::start(int interval_ms) {
    if (running_.load()) return false;
    interval_ms_ = interval_ms;
    running_.store(true);
    worker_ = std::thread(&IopsMonitor::sampling_loop, this);
    return true;
}

void IopsMonitor::stop() {
    running_.store(false);
    if (worker_.joinable())
        worker_.join();
}

void IopsMonitor::sampling_loop() {
    while (running_.load()) {
        auto start = std::chrono::steady_clock::now();

#ifdef _WIN32
        collect_iops_windows();
#elif __linux__
        collect_iops_linux();
#else
        collect_iops_synthetic();
#endif

        samples_collected_.fetch_add(1);
        if (sample_cb_)
            sample_cb_(samples_collected_.load());

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        int sleep_ms = interval_ms_ - static_cast<int>(elapsed);
        if (sleep_ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
    }
}

void IopsMonitor::collect_iops_windows() {
    if (!catalog_ || !heat_map_) return;

    auto extents = catalog_->all_extents();
    for (auto& ext : extents) {
        int64_t fake_iops = 1 + (std::rand() % 20);
        int64_t fake_lba = ext.offset_bytes / 512;
        heat_map_->record_iops(ext.id, fake_iops, fake_iops / 2);
        heat_map_->record_lba(ext.id, fake_lba);
    }
}

void IopsMonitor::collect_iops_linux() {
    collect_iops_synthetic();
}

void IopsMonitor::collect_iops_synthetic() {
    if (!catalog_ || !heat_map_) return;

    auto extents = catalog_->all_extents();
    static std::mt19937 rng(std::random_device{}());
    static std::uniform_int_distribution<int> iops_dist(0, 30);
    static std::uniform_int_distribution<int> lba_dist(0, 1000000);

    for (auto& ext : extents) {
        int64_t iops = iops_dist(rng);
        int64_t lba = lba_dist(rng);
        if (iops > 0) {
            heat_map_->record_iops(ext.id, iops * 3 / 4, iops / 4);
            heat_map_->record_lba(ext.id, lba);
        }
    }
}
