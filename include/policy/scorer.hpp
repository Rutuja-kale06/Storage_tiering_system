#pragma once

#include "core/file_record.hpp"
#include <string>
#include <memory>
#include <vector>
#include <nlohmann/json.hpp>

struct ScoringContext {
    double hot_threshold     = 15.0;
    double warm_threshold    = 0.0;
    double cold_threshold    = -15.0;
    double archive_threshold = -100.0;
};

class Scorer {
public:
    virtual ~Scorer() = default;
    virtual std::string name() const = 0;
    virtual double score(const FileRecord& file, const ScoringContext& ctx) const = 0;
    virtual std::string explain(const FileRecord& file) const = 0;
    virtual bool load_config(const nlohmann::json& config) = 0;
};
