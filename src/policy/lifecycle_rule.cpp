#include "policy/lifecycle_rule.hpp"
#include "core/types.hpp"
#include <algorithm>
#include <filesystem>
#include <regex>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

static std::regex glob_to_regex(const std::string& pattern) {
    std::string re = "^";
    for (size_t i = 0; i < pattern.size(); ++i) {
        char c = pattern[i];
        switch (c) {
            case '*':
                if (i + 1 < pattern.size() && pattern[i + 1] == '*') {
                    re += ".*";
                    ++i;
                } else {
                    re += "[^/]*";
                }
                break;
            case '?': re += "[^/]"; break;
            case '.': re += "\\."; break;
            case '/': re += "[/\\\\]"; break;
            case '\\': re += "\\\\"; break;
            default:  re += c;
        }
    }
    re += "$";
    return std::regex(re, std::regex::icase);
}

static bool path_matches_glob(const std::string& path, const std::string& pattern) {
    static thread_local std::unordered_map<std::string, std::regex> cache;
    auto it = cache.find(pattern);
    if (it == cache.end())
        it = cache.emplace(pattern, glob_to_regex(pattern)).first;
    return std::regex_search(path, it->second);
}

bool LifecycleRule::matches(const FileRecord& file) const {
    // File type filter
    if (!file_types.empty()) {
        bool type_match = false;
        for (auto ft : file_types) {
            if (file.file_type == ft) { type_match = true; break; }
        }
        if (!type_match) return false;
    }

    // Age filters
    double age = file.age_days();
    if (min_age_days > 0 && age < min_age_days) return false;
    if (max_age_days > 0 && age > max_age_days) return false;

    // Size filters
    if (min_size_bytes > 0 && file.size_bytes < min_size_bytes) return false;
    if (max_size_bytes < INT64_MAX && file.size_bytes > max_size_bytes) return false;

    // Access count filters
    if (min_access_count > 0 && file.access_count < min_access_count) return false;
    if (max_access_count < INT32_MAX && file.access_count > max_access_count) return false;

    // Path patterns
    if (!path_patterns.empty()) {
        bool path_match = false;
        for (const auto& pattern : path_patterns) {
            if (path_matches_glob(file.path, pattern)) {
                path_match = true;
                break;
            }
        }
        if (!path_match) return false;
    }

    return true;
}

std::string LifecycleRule::description() const {
    std::ostringstream oss;
    oss << id << " [priority=" << priority << "]: ";
    if (!file_types.empty()) {
        oss << "type in {";
        for (size_t i = 0; i < file_types.size(); ++i) {
            if (i > 0) oss << ",";
            oss << file_type_name(file_types[i]);
        }
        oss << "} ";
    }
    if (min_age_days > 0) oss << "age>" << min_age_days << "d ";
    if (max_age_days > 0) oss << "age<" << max_age_days << "d ";
    switch (action) {
        case LifecycleAction::MIGRATE_TO_TIER:
            oss << "→ migrate to " << tier_name(target_tier);
            break;
        case LifecycleAction::DELETE_FILE:
            oss << "→ delete";
            break;
        case LifecycleAction::PIN_TO_TIER:
            oss << "→ pin to " << tier_name(target_tier);
            break;
    }
    return oss.str();
}

// ── LifecycleEngine ──────────────────────────────────────────

bool LifecycleEngine::load_rules(const std::string& yaml_path) {
    try {
        YAML::Node root = YAML::LoadFile(yaml_path);
        return load_rules_from_yaml(root);
    } catch (const std::exception& e) {
        return false;
    }
}

bool LifecycleEngine::load_rules_from_yaml(const YAML::Node& node) {
    rules_.clear();
    if (!node["lifecycle_rules"] || !node["lifecycle_rules"].IsSequence())
        return false;

    for (const auto& rule_node : node["lifecycle_rules"]) {
        LifecycleRule rule;
        rule.id       = rule_node["id"].as<std::string>();
        rule.priority = rule_node["priority"].as<int>(0);

        // Filter
        if (rule_node["filter"]) {
            auto& filter = rule_node["filter"];
            if (filter["file_type"]) {
                if (filter["file_type"].IsScalar()) {
                    // Single type
                    rule.file_types.push_back(
                        static_cast<FileType>(filter["file_type"].as<int>()));
                } else {
                    for (auto& ft : filter["file_type"])
                        rule.file_types.push_back(static_cast<FileType>(ft.as<int>()));
                }
            }
            if (filter["older_than_days"])
                rule.min_age_days = filter["older_than_days"].as<double>();
            if (filter["newer_than_days"])
                rule.max_age_days = filter["newer_than_days"].as<double>();
            if (filter["larger_than_bytes"])
                rule.min_size_bytes = filter["larger_than_bytes"].as<int64_t>();
            if (filter["smaller_than_bytes"])
                rule.max_size_bytes = filter["smaller_than_bytes"].as<int64_t>();
            if (filter["min_accesses"])
                rule.min_access_count = filter["min_accesses"].as<int>();
            if (filter["path_pattern"] || filter["path_patterns"]) {
                auto& pp = filter["path_pattern"] ? filter["path_pattern"] : filter["path_patterns"];
                if (pp.IsScalar())
                    rule.path_patterns.push_back(pp.as<std::string>());
                else
                    for (auto& p : pp) rule.path_patterns.push_back(p.as<std::string>());
            }
        }

        // Action
        if (rule_node["action"]) {
            std::string action_str = rule_node["action"].as<std::string>();
            if (action_str.find("migrate_to(") != std::string::npos) {
                rule.action = LifecycleAction::MIGRATE_TO_TIER;
                // Extract tier name from "migrate_to(HOT)"
                auto start = action_str.find('(') + 1;
                auto end = action_str.find(')');
                std::string tier_name = action_str.substr(start, end - start);
                for (int i = 0; i < TIER_COUNT; ++i) {
                    if (tier_name == TIER_NAMES[i]) {
                        rule.target_tier = static_cast<Tier>(i);
                        break;
                    }
                }
            } else if (action_str.find("pin(") != std::string::npos) {
                rule.action = LifecycleAction::PIN_TO_TIER;
                auto start = action_str.find('(') + 1;
                auto end = action_str.find(')');
                std::string tier_name = action_str.substr(start, end - start);
                for (int i = 0; i < TIER_COUNT; ++i) {
                    if (tier_name == TIER_NAMES[i]) {
                        rule.target_tier = static_cast<Tier>(i);
                        break;
                    }
                }
            } else if (action_str == "delete()") {
                rule.action = LifecycleAction::DELETE_FILE;
            }
        }

        rules_.push_back(rule);
    }

    // Sort by priority descending
    std::sort(rules_.begin(), rules_.end(),
        [](const LifecycleRule& a, const LifecycleRule& b) {
            return a.priority > b.priority;
        });

    return true;
}

void LifecycleEngine::clear_rules() {
    rules_.clear();
}

std::vector<const LifecycleRule*> LifecycleEngine::matching_rules(const FileRecord& file) const {
    std::vector<const LifecycleRule*> matches;
    for (const auto& rule : rules_) {
        if (rule.matches(file))
            matches.push_back(&rule);
    }
    return matches;
}

std::vector<LifecycleEngine::RuleAction> LifecycleEngine::evaluate(
    const std::vector<FileRecord>& files) const
{
    std::vector<RuleAction> actions;
    for (const auto& file : files) {
        auto matching = matching_rules(file);
        if (!matching.empty()) {
            // Take the highest-priority matching rule
            actions.push_back({matching.front(), file.id});
        }
    }
    return actions;
}
