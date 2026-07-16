#include "policy/recency_scorer.hpp"
#include "logger.hpp"
#include <sstream>
#include <iomanip>

RecencyScorer::RecencyScorer() {
    thresholds_ = {
        {  1.0,  20.0 },
        {  3.0,  12.0 },
        {  7.0,   5.0 },
        { 14.0,  -5.0 },
        { 30.0, -12.0 },
        { 1e9,  -22.0 }   // infinity sentinel
    };
}

double RecencyScorer::score(const FileRecord& file, const ScoringContext& ctx) const {
    double idle = file.idle_days();
    for (const auto& t : thresholds_) {
        if (idle < t.max_idle_days)
            return t.weight;
    }
    return thresholds_.back().weight;
}

std::string RecencyScorer::explain(const FileRecord& file) const {
    double idle = file.idle_days();
    double s = score(file, {});
    std::ostringstream oss;
    oss << "recency=" << (s >= 0 ? "+" : "") << std::fixed << std::setprecision(1)
        << s << " (idle=" << std::setprecision(1) << idle << "d)";
    return oss.str();
}

bool RecencyScorer::load_config(const nlohmann::json& config) {
    if (config.contains("thresholds") && config["thresholds"].is_array()) {
        thresholds_.clear();
        for (const auto& t : config["thresholds"]) {
            Threshold th;
            th.max_idle_days = t.value("max_idle_hours", 24.0) / 24.0;
            th.weight        = t.value("weight", 0.0);
            thresholds_.push_back(th);
        }
        // Ensure sentinel exists
        if (thresholds_.empty() || thresholds_.back().max_idle_days < 1e8) {
            thresholds_.push_back({1e9, -22.0});
        }
    }
    return true;
}

void RecencyScorer::log_warning_if_default() const {}
