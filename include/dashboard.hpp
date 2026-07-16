#pragma once

#include "core/file_record.hpp"
#include "core/types.hpp"
#include "policy/policy_engine.hpp"
#include "migration/migration_engine.hpp"
#include "catalog/catalog_interface.hpp"
#include <memory>

class Dashboard {
public:
    Dashboard(std::shared_ptr<CatalogInterface> catalog,
              std::shared_ptr<PolicyEngine> policy,
              std::shared_ptr<MigrationEngine> engine);

    void clear();
    void print_banner();
    void print_tier_summary();
    void print_recent_migrations(int n = 8);
    void print_file_list(Tier t, int limit = 12);
    void print_analysis();
    void print_help();
    void print_savings_report();
    void print_system_status();
    void print_engine_state();

    // Interactive file access (like the original)
    void access_file_interactive();
    void add_file_interactive();
    void pin_file_interactive();

private:
    std::shared_ptr<CatalogInterface> catalog_;
    std::shared_ptr<PolicyEngine>      policy_;
    std::shared_ptr<MigrationEngine>   engine_;

    void hline(char c = '-', const std::string& color = DIM);
    void hline_double(const std::string& color = CYAN);
    std::string progress_bar(double frac, int width, const std::string& col);
};
