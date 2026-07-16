#include "migration/migration_engine.hpp"
#include "core/types.hpp"
#include <sstream>
#include <chrono>
#include <algorithm>
#include <future>

MigrationEngine::MigrationEngine(std::shared_ptr<CatalogInterface> catalog,
                                 std::shared_ptr<PolicyEngine> policy)
    : catalog_(std::move(catalog))
    , policy_(std::move(policy))
{
    // Register default local migrator (root is current directory)
    register_migrator(std::make_unique<LocalMigrator>("."));
}

void MigrationEngine::register_migrator(std::unique_ptr<Migrator> migrator) {
    migrators_.push_back(std::move(migrator));
}

std::string MigrationEngine::state_name() const {
    switch (state_.load()) {
        case State::IDLE:          return "IDLE";
        case State::ANALYSING:     return "ANALYSING";
        case State::PLANNING:      return "PLANNING";
        case State::MIGRATING:     return "MIGRATING";
        case State::CHECKPOINTING: return "CHECKPOINTING";
        case State::PAUSED:        return "PAUSED";
        case State::ERROR:         return "ERROR";
    }
    return "UNKNOWN";
}

void MigrationEngine::pause() {
    pause_requested_.store(true);
}

void MigrationEngine::resume() {
    pause_requested_.store(false);
    State expected = State::PAUSED;
    state_.compare_exchange_strong(expected, State::IDLE);
}

void MigrationEngine::cancel() {
    cancel_requested_.store(true);
    resume();
}

bool MigrationEngine::is_running() const {
    State s = state_.load();
    return s == State::ANALYSING || s == State::PLANNING ||
           s == State::MIGRATING || s == State::CHECKPOINTING;
}

std::vector<size_t> MigrationEngine::find_migrators_for_file(const FileRecord& file) const {
    std::vector<size_t> result;
    for (size_t i = 0; i < migrators_.size(); ++i) {
        if (migrators_[i]->can_migrate(file, file.current_tier))
            result.push_back(i);
    }
    return result;
}

MigrateResult MigrationEngine::execute_migration(MigrationJob& job) {
    // Get the file record
    auto file_opt = catalog_->get_file(job.file_id);
    if (!file_opt.has_value())
        return {false, "", "File not found in catalog"};

    FileRecord file = file_opt.value();

    // Find suitable migrator
    auto indices = find_migrators_for_file(file);
    if (indices.empty())
        return {false, "", "No migrator available for this file"};

    // Execute with first suitable migrator
    auto result = migrators_[indices[0]]->migrate(file, job.to_tier);
    if (result.success) {
        // Update catalog
        file.current_tier = job.to_tier;
        file.migrate_count++;
        file.target_tier = job.to_tier;
        catalog_->update_file(file);

        // Log migration event
        MigrationEvent event;
        event.file_id    = file.id;
        event.file_path  = file.path;
        event.from_tier  = job.from_tier;
        event.to_tier    = job.to_tier;
        event.size_bytes = file.size_bytes;
        event.reason     = job.reason;
        event.timestamp  = std::time(nullptr);
        event.success    = true;
        event.duration_ms = result.duration_ms;
        catalog_->log_migration(event);

        total_migrations_.fetch_add(1);
        total_bytes_migrated_.fetch_add(file.size_bytes);
    }

    return result;
}

void MigrationEngine::apply_lifecycle_actions(
    const std::vector<LifecycleEngine::RuleAction>& actions)
{
    for (const auto& action : actions) {
        if (cancel_requested_.load()) break;

        auto file_opt = catalog_->get_file(action.file_id);
        if (!file_opt) continue;

        FileRecord file = file_opt.value();

        switch (action.rule->action) {
            case LifecycleAction::MIGRATE_TO_TIER: {
                file.target_tier = action.rule->target_tier;
                catalog_->update_file(file);
                break;
            }
            case LifecycleAction::DELETE_FILE: {
                // Remove from filesystem and catalog
                try {
                    if (std::filesystem::exists(file.path))
                        std::filesystem::remove(file.path);
                } catch (...) {}
                catalog_->delete_file(file.id);
                break;
            }
            case LifecycleAction::PIN_TO_TIER: {
                file.is_pinned = true;
                file.current_tier = action.rule->target_tier;
                catalog_->update_file(file);
                break;
            }
        }
    }
}

CycleResult MigrationEngine::run_cycle() {
    CycleResult result;
    auto cycle_start = std::chrono::steady_clock::now();

    state_.store(State::ANALYSING);

    // Step 1: Analyse
    auto recommendations = policy_->analyse(*catalog_);
    result.total_recommended = static_cast<int>(recommendations.size());

    if (recommendations.empty()) {
        state_.store(State::IDLE);
        auto cycle_end = std::chrono::steady_clock::now();
        result.duration_ms = std::chrono::duration<double, std::milli>(
            cycle_end - cycle_start).count();
        return result;
    }

    // Step 2: Apply lifecycle rules
    state_.store(State::PLANNING);
    auto all_files = catalog_->all_files();
    auto lifecycle_actions = policy_->apply_lifecycle(all_files);
    if (!lifecycle_actions.empty()) {
        apply_lifecycle_actions(lifecycle_actions);
    }

    // Step 3: Plan
    auto plan = planner_.plan(recommendations);
    if (plan.jobs.empty()) {
        state_.store(State::IDLE);
        auto cycle_end = std::chrono::steady_clock::now();
        result.duration_ms = std::chrono::duration<double, std::milli>(
            cycle_end - cycle_start).count();
        return result;
    }

    // Check maintenance window
    if (!planner_.within_maintenance_window()) {
        result.window_exceeded = true;
        state_.store(State::IDLE);
        return result;
    }

    // Step 4: Migrate
    state_.store(State::MIGRATING);
    auto batches = planner_.batch_jobs(plan);

    for (auto& batch : batches) {
        if (cancel_requested_.load()) break;

        // Check pause
        if (pause_requested_.load()) {
            state_.store(State::PAUSED);
            // Wait for resume
            while (pause_requested_.load() && !cancel_requested_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (cancel_requested_.load()) break;
            state_.store(State::MIGRATING);
        }

        // Execute batch in parallel
        std::vector<std::future<std::pair<MigrationJob, MigrateResult>>> futures;
        for (auto& job : batch) {
            if (cancel_requested_.load()) break;
            futures.push_back(std::async(std::launch::async,
                [this, &job]() -> std::pair<MigrationJob, MigrateResult> {
                    return {job, execute_migration(job)};
                }));
        }
        for (auto& f : futures) {
            auto [job, migrate_result] = f.get();
            if (migrate_result.success) {
                result.migrated++;
                result.bytes_migrated += job.size_bytes;
                result.estimated_monthly_savings += job.estimated_savings_per_month;
            } else {
                result.failed++;
            }
        }
    }

    // Step 5: Checkpoint
    state_.store(State::CHECKPOINTING);
    cycle_count_.fetch_add(1);
    save_checkpoint(plan);

    state_.store(State::IDLE);

    auto cycle_end = std::chrono::steady_clock::now();
    result.duration_ms = std::chrono::duration<double, std::milli>(
        cycle_end - cycle_start).count();

    return result;
}

void MigrationEngine::run_cycle_async(std::function<void(const CycleResult&)> callback) {
    if (is_running()) return;

    if (worker_.joinable())
        worker_.join();

    worker_ = std::thread([this, callback = std::move(callback)]() {
        auto result = run_cycle();
        if (callback) callback(result);
    });
    worker_.detach();
}

bool MigrationEngine::save_checkpoint(const MigrationPlan& plan) {
    // In production: serialize plan to JSON, store in DB metadata table
    return true;
}

std::optional<MigrationPlan> MigrationEngine::load_checkpoint() {
    // In production: deserialize from DB metadata table
    return std::nullopt;
}

bool MigrationEngine::clear_checkpoint() {
    return true;
}

double MigrationEngine::savings_vs_all_hot(const CatalogInterface& catalog) const {
    double actual = catalog.total_monthly_cost();
    int64_t total_bytes = catalog.total_bytes();
    double all_hot_cost = (total_bytes / (1024.0 * 1024.0 * 1024.0))
                          * tier_cost_per_gb(Tier::HOT) * 30.0;
    return all_hot_cost - actual;
}

double MigrationEngine::savings_percent(const CatalogInterface& catalog) const {
    double actual = catalog.total_monthly_cost();
    int64_t total_bytes = catalog.total_bytes();
    double max_cost = (total_bytes / (1024.0 * 1024.0 * 1024.0))
                      * tier_cost_per_gb(Tier::HOT) * 30.0;
    if (max_cost <= 0) return 0.0;
    return (max_cost - actual) / max_cost * 100.0;
}

std::vector<MigrationEvent> MigrationEngine::recent_history(int n) const {
    return catalog_->recent_migrations(n);
}
