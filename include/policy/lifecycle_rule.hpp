#pragma once

#include "core/file_record.hpp"
#include <string>
#include <vector>
#include <functional>
#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

enum class LifecycleAction {
    MIGRATE_TO_TIER,
    DELETE_FILE,
    PIN_TO_TIER
};

struct LifecycleRule {
    std::string id;
    int priority = 0;

    // Filter criteria
    std::vector<FileType> file_types;       // empty = any
    double min_age_days = 0;                 // 0 = any
    double max_age_days = 0;                 // 0 = no max
    int64_t min_size_bytes = 0;              // 0 = any
    int64_t max_size_bytes = INT64_MAX;      // INT64_MAX = no max
    int min_access_count = 0;
    int max_access_count = INT32_MAX;
    std::vector<std::string> path_patterns;  // glob patterns (empty = any)

    // Action
    LifecycleAction action;
    Tier target_tier = Tier::HOT;            // for MIGRATE/PIN

    bool matches(const FileRecord& file) const;
    std::string description() const;
};

class LifecycleEngine {
public:
    bool load_rules(const std::string& yaml_path);
    bool load_rules_from_yaml(const YAML::Node& node);
    void clear_rules();

    // Returns rules that match this file, sorted by priority descending
    std::vector<const LifecycleRule*> matching_rules(const FileRecord& file) const;

    // Evaluate all rules against all files, return actions to take
    struct RuleAction {
        const LifecycleRule* rule;
        std::string file_id;
    };
    std::vector<RuleAction> evaluate(const std::vector<FileRecord>& files) const;

    const std::vector<LifecycleRule>& rules() const { return rules_; }

private:
    std::vector<LifecycleRule> rules_;
};
