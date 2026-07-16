#include "policy/type_scorer.hpp"
#include <sstream>
#include <iomanip>

TypeScorer::TypeScorer() {
    type_weights_ = {
        { static_cast<int>(FileType::DATABASE),    15.0 },
        { static_cast<int>(FileType::CONFIG),      10.0 },
        { static_cast<int>(FileType::EXECUTABLE),   8.0 },
        { static_cast<int>(FileType::ANALYTICS),    5.0 },
        { static_cast<int>(FileType::LOGS),        -5.0 },
        { static_cast<int>(FileType::MEDIA),       -6.0 },
        { static_cast<int>(FileType::ARCHIVE_ZIP), -8.0 },
        { static_cast<int>(FileType::BACKUP),     -10.0 },
        { static_cast<int>(FileType::TEMPORARY),  -15.0 },
        { static_cast<int>(FileType::OTHER),        0.0 },
    };
}

double TypeScorer::score(const FileRecord& file, const ScoringContext& ctx) const {
    auto it = type_weights_.find(static_cast<int>(file.file_type));
    return it != type_weights_.end() ? it->second : 0.0;
}

std::string TypeScorer::explain(const FileRecord& file) const {
    double s = score(file, {});
    std::ostringstream oss;
    oss << "type=" << (s >= 0 ? "+" : "") << std::fixed << std::setprecision(1) << s
        << " (" << file_type_name(file.file_type) << ")";
    return oss.str();
}

bool TypeScorer::load_config(const nlohmann::json& config) {
    if (config.contains("type_weights") && config["type_weights"].is_object()) {
        type_weights_.clear();
        for (auto it = config["type_weights"].begin(); it != config["type_weights"].end(); ++it) {
            // Map string name to FileType
            std::string name = it.key();
            double weight = it.value().get<double>();
            for (int i = 0; i < FILE_TYPE_COUNT; ++i) {
                if (file_type_name(static_cast<FileType>(i)) == name ||
                    FILE_TYPE_NAMES[i] == name) {
                    type_weights_[i] = weight;
                    break;
                }
            }
        }
    }
    return true;
}
