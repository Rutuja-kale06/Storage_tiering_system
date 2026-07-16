#pragma once

#include "migration_planner.hpp"
#include "local_migrator.hpp"
#include "policy/policy_engine.hpp"
#include "catalog/catalog_interface.hpp"
#include <memory>
#include <atomic>
#include <thread>
#include <functional>

struct CycleResult {
    int     total_recommended = 0;
    int     migrated = 0;
    int     failed = 0;
    int64_t bytes_migrated = 0;
    double  duration_ms = 0.0;
    double  estimated_monthly_savings = 0.0;
    bool    was_throttled = false;
    bool    window_exceeded = false;
};

class MigrationEngine {
public:
    MigrationEngine(std::shared_ptr<CatalogInterface> catalog,
                    std::shared_ptr<PolicyEngine> policy);

    // State machine
    enum class State {
        IDLE,
        ANALYSING,
        PLANNING,
        MIGRATING,
        CHECKPOINTING,
        PAUSED,
        ERROR
    };

    State state() const { return state_.load(); }
    std::string state_name() const;

    // Register a migrator backend (by URI scheme)
    void register_migrator(std::unique_ptr<Migrator> migrator);

    // Run a full cycle: analyse → plan → migrate
    CycleResult run_cycle();

    // Run in background thread
    void run_cycle_async(std::function<void(const CycleResult&)> callback = nullptr);

    // Control
    void pause();
    void resume();
    void cancel();
    bool is_running() const;

    // Checkpoint/resume
    bool save_checkpoint(const MigrationPlan& plan);
    std::optional<MigrationPlan> load_checkpoint();
    bool clear_checkpoint();

    // Accessors
    int  cycle_count() const { return cycle_count_.load(); }
    int  total_migrations() const { return total_migrations_.load(); }
    int64_t total_bytes_migrated() const { return total_bytes_migrated_.load(); }

    // Cost savings
    double savings_vs_all_hot(const CatalogInterface& catalog) const;
    double savings_percent(const CatalogInterface& catalog) const;

    // History
    std::vector<MigrationEvent> recent_history(int n = 10) const;

private:
    std::shared_ptr<CatalogInterface> catalog_;
    std::shared_ptr<PolicyEngine>      policy_;
    MigrationPlanner                   planner_;

    std::vector<std::unique_ptr<Migrator>> migrators_;

    std::atomic<State> state_{State::IDLE};
    std::atomic<int>   cycle_count_{0};
    std::atomic<int>   total_migrations_{0};
    std::atomic<int64_t> total_bytes_migrated_{0};

    std::thread worker_;
    std::atomic<bool> cancel_requested_{false};
    std::atomic<bool> pause_requested_{false};

    std::vector<size_t> find_migrators_for_file(const FileRecord& file) const;
    MigrateResult execute_migration(MigrationJob& job);
    void apply_lifecycle_actions(const std::vector<LifecycleEngine::RuleAction>& actions);
};
