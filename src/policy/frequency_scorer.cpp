#include "policy/frequency_scorer.hpp"
#include <algorithm>
#include <sstream>
#include <iomanip>

FrequencyScorer::FrequencyScorer() = default;

double FrequencyScorer::score(const FileRecord& file, const ScoringContext& ctx) const {
    if (file.access_count == 0) return 0.0;
    double rate = file.access_rate();
    return std::min(rate * multiplier_, max_bonus_);
}

std::string FrequencyScorer::explain(const FileRecord& file) const {
    double s = score(file, {});
    std::ostringstream oss;
    oss << "freq=" << (s >= 0 ? "+" : "") << std::fixed << std::setprecision(1) << s
        << " (rate=" << std::setprecision(2) << file.access_rate() << "/day)";
    return oss.str();
}

bool FrequencyScorer::load_config(const nlohmann::json& config) {
    if (config.contains("accesses_per_day_multiplier"))
        multiplier_ = config["accesses_per_day_multiplier"].get<double>();
    if (config.contains("max_bonus"))
        max_bonus_ = config["max_bonus"].get<double>();
    return true;
}
