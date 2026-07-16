#pragma once

#include "heat_map.hpp"
#include <string>
#include <thread>
#include <atomic>
#include <memory>
#include <functional>

class CatalogInterface;

class IopsMonitor {
public:
    IopsMonitor();
    ~IopsMonitor();

    bool start(int interval_ms = 5000);
    void stop();
    bool is_running() const { return running_.load(); }

    void set_heat_map(std::shared_ptr<HeatMapEngine> heat_map) { heat_map_ = heat_map; }
    void set_catalog(CatalogInterface* catalog) { catalog_ = catalog; }

    int  samples_collected() const { return samples_collected_.load(); }
    void reset_stats() { samples_collected_.store(0); }

    using SampleCallback = std::function<void(int extents_sampled)>;
    void on_sample(SampleCallback cb) { sample_cb_ = std::move(cb); }

private:
    std::atomic<bool> running_{false};
    std::thread worker_;
    int interval_ms_ = 5000;
    std::shared_ptr<HeatMapEngine> heat_map_;
    CatalogInterface* catalog_ = nullptr;
    std::atomic<int> samples_collected_{0};
    SampleCallback sample_cb_;

    void sampling_loop();
    void collect_iops_windows();
    void collect_iops_linux();
    void collect_iops_synthetic();
};
