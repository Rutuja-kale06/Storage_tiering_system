#pragma once

#include "scorer.hpp"
#include <unordered_map>

class TypeScorer : public Scorer {
public:
    TypeScorer();

    std::string name() const override { return "file_type"; }
    double score(const FileRecord& file, const ScoringContext& ctx) const override;
    std::string explain(const FileRecord& file) const override;
    bool load_config(const nlohmann::json& config) override;

private:
    std::unordered_map<int, double> type_weights_;
};
