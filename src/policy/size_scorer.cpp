#include "policy/size_scorer.hpp"
#include <sstream>
#include <iomanip>

SizeScorer::SizeScorer() = default;

double SizeScorer::score(const FileRecord& file, const ScoringContext& ctx) const {
    if (file.size_gb() > huge_size_gb_)
        return huge_penalty_;
    if (file.size_gb() > large_size_gb_)
        return large_penalty_;
    return 0.0;
}

std::string SizeScorer::explain(const FileRecord& file) const {
    double s = score(file, {});
    std::ostringstream oss;
    oss << "size=" << (s >= 0 ? "+" : "") << std::fixed << std::setprecision(1) << s
        << " (" << std::setprecision(2) << file.size_gb() << " GB)";
    return oss.str();
}

bool SizeScorer::load_config(const nlohmann::json& config) {
    if (config.contains("large_size_gb"))
        large_size_gb_  = config["large_size_gb"].get<double>();
    if (config.contains("huge_size_gb"))
        huge_size_gb_   = config["huge_size_gb"].get<double>();
    if (config.contains("large_penalty"))
        large_penalty_  = config["large_penalty"].get<double>();
    if (config.contains("huge_penalty"))
        huge_penalty_   = config["huge_penalty"].get<double>();
    return true;
}
