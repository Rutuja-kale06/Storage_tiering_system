#include "dashboard.hpp"
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

Dashboard::Dashboard(std::shared_ptr<CatalogInterface> catalog,
                     std::shared_ptr<PolicyEngine> policy,
                     std::shared_ptr<MigrationEngine> engine)
    : catalog_(std::move(catalog))
    , policy_(std::move(policy))
    , engine_(std::move(engine))
{}

void Dashboard::clear() {
    std::cout << "\033[2J\033[H" << std::flush;
}

void Dashboard::hline(char c, const std::string& color) {
    std::cout << color << std::string(76, c) << RESET << "\n";
}

void Dashboard::hline_double(const std::string& color) {
    std::cout << color << std::string(76, '=') << RESET << "\n";
}

std::string Dashboard::progress_bar(double frac, int width, const std::string& col) {
    frac = std::max(0.0, std::min(1.0, frac));
    int filled = static_cast<int>(frac * width);
    int empty  = width - filled;
    return col + std::string(filled, '#') +
           DIM + std::string(empty, '.') + RESET;
}

void Dashboard::print_banner() {
    hline_double(CYAN);
    std::string title = "  AUTO STORAGE TIERING SYSTEM  v2.0";
    std::string ts = format_time(time(nullptr));
    int pad = 76 - static_cast<int>(title.size()) - static_cast<int>(ts.size()) - 2;
    std::cout << BOLD << CYAN << title
              << std::string(std::max(pad, 1), ' ')
              << DIM << ts << RESET << "\n";
    hline_double(CYAN);
}

void Dashboard::print_tier_summary() {
    int64_t total_bytes = catalog_->total_bytes();
    if (total_bytes == 0) total_bytes = 1;

    std::cout << "\n" << BOLD << "  STORAGE TIERS\n" << RESET;
    hline();

    for (int i = 0; i < TIER_COUNT; ++i) {
        Tier t = static_cast<Tier>(i);
        TierStats s = catalog_->stats_for_tier(t);
        double pct = static_cast<double>(s.total_bytes) / total_bytes * 100.0;
        std::string bar = progress_bar(pct / 100.0, 20, tier_color(t));

        std::cout << "  " << tier_color(t) << BOLD << tier_label(t) << RESET
                  << "  " << bar
                  << "  " << std::setw(6) << std::fixed << std::setprecision(1) << pct << "%"
                  << "  " << std::setw(10) << format_bytes(s.total_bytes)
                  << "  " << std::setw(4) << s.file_count << " files"
                  << "  $" << std::fixed << std::setprecision(2)
                  << std::setw(8) << s.monthly_cost_usd << "/mo\n";
        std::cout << "  " << DIM
                  << std::string(tier_label(t).size(), ' ')
                  << "  Latency: " << std::setw(7) << tier_latency_ms(t) << " ms"
                  << "  Cost: $" << std::setprecision(3) << tier_cost_per_gb(t)
                  << "/GB/month\n" << RESET;
    }
    hline();

    double total_cost = catalog_->total_monthly_cost();
    double savings = engine_->savings_vs_all_hot(*catalog_);
    double sav_pct = engine_->savings_percent(*catalog_);

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  " << BOLD << "Total managed: "
              << format_bytes(catalog_->total_bytes()) << "  ("
              << catalog_->file_count() << " files)"
              << "   Monthly cost: $" << total_cost << RESET << "\n";
    std::cout << "  " << GREEN << BOLD
              << "  Savings vs all-HOT: $" << savings
              << "/mo  (" << sav_pct << "%)" << RESET << "\n";
}

void Dashboard::print_recent_migrations(int n) {
    std::cout << "\n" << BOLD << "  RECENT MIGRATIONS"
              << "  (total: " << engine_->total_migrations() << "  |  "
              << "bytes moved: " << format_bytes(engine_->total_bytes_migrated())
              << ")\n" << RESET;
    hline();

    auto events = engine_->recent_history(n);
    if (events.empty()) {
        std::cout << "  " << DIM << "No migrations yet.\n" << RESET;
        return;
    }

    for (auto it = events.rbegin(); it != events.rend(); ++it) {
        auto& e = *it;
        std::string name = e.file_path;
        if (name.size() > 28) name = "..." + name.substr(name.size() - 27);

        std::cout << "  " << DIM << e.time_str() << RESET
                  << "  " << tier_color(e.from_tier)
                  << pad_right(tier_name(e.from_tier), 5) << RESET
                  << " -> " << tier_color(e.to_tier)
                  << pad_right(tier_name(e.to_tier), 7) << RESET
                  << "  " << std::setw(10) << format_bytes(e.size_bytes)
                  << "  " << DIM << pad_right(name, 30) << RESET;
        if (!e.success)
            std::cout << "  " << RED << "[FAILED]" << RESET;
        std::cout << "\n";
    }
}

void Dashboard::print_file_list(Tier t, int limit) {
    auto files = catalog_->files_by_tier(t);
    std::string col = tier_color(t);

    std::cout << "\n" << BOLD << col
              << "  FILES ON " << tier_name(t) << " TIER"
              << "  (" << files.size() << " files)\n" << RESET;
    hline();

    if (files.empty()) {
        std::cout << "  " << DIM << "No files.\n" << RESET;
        return;
    }

    std::sort(files.begin(), files.end(),
        [](const FileRecord& a, const FileRecord& b) {
            return a.idle_days() > b.idle_days();
        });

    std::cout << "  " << BOLD
              << pad_right("Filename", 30)
              << pad_left("Size", 10)
              << pad_left("Idle(d)", 8)
              << pad_left("Accesses", 10)
              << pad_left("Type", 12)
              << pad_left("Score", 7)
              << "\n" << RESET;
    hline('-');

    int shown = 0;
    for (auto& f : files) {
        if (++shown > limit) break;
        std::string name = f.path;
        if (name.size() > 29) name = "..." + name.substr(name.size() - 28);
        double sc = policy_->score_file(f);

        std::cout << "  " << pad_right(name, 30)
                  << pad_left(format_bytes(f.size_bytes), 10)
                  << std::fixed << std::setprecision(1)
                  << pad_left(std::to_string(static_cast<int>(f.idle_days())), 8)
                  << pad_left(std::to_string(f.access_count), 10)
                  << pad_left(file_type_name(f.file_type), 12)
                  << std::fixed << std::setprecision(1)
                  << pad_left((sc >= 0 ? "+" : "") +
                              std::to_string(static_cast<int>(sc * 10) / 10.0), 7)
                  << "\n";
    }
    if (static_cast<int>(files.size()) > limit)
        std::cout << "  " << DIM << "  ... and " << files.size() - limit
                  << " more\n" << RESET;
}

void Dashboard::print_analysis() {
    auto recs = policy_->analyse(*catalog_);

    std::cout << "\n" << BOLD << "  DRY-RUN ANALYSIS  —  "
              << recs.size() << " file(s) would be migrated\n" << RESET;
    hline();

    if (recs.empty()) {
        std::cout << "  " << GREEN
                  << "All files are on the optimal tier.\n" << RESET;
        return;
    }

    std::cout << "  " << BOLD
              << pad_right("File", 30)
              << pad_left("Size", 10)
              << pad_left("From", 7)
              << pad_left("To", 9)
              << pad_left("Savings/mo", 12)
              << "\n" << RESET;
    hline('-');

    double savings = 0;
    for (auto& r : recs) {
        std::string name = r.file.path;
        if (name.size() > 29) name = "..." + name.substr(name.size() - 28);

        std::string fc = tier_color(r.file.current_tier);
        std::string tc = tier_color(r.target_tier);

        std::cout << "  " << pad_right(name, 30)
                  << pad_left(format_bytes(r.file.size_bytes), 10)
                  << "  " << fc << pad_right(tier_name(r.file.current_tier), 5) << RESET
                  << "  " << tc << pad_right(tier_name(r.target_tier), 7) << RESET
                  << "$" << std::fixed << std::setprecision(2)
                  << pad_left(std::to_string(r.estimated_monthly_savings), 9)
                  << "\n";
        savings += r.estimated_monthly_savings;
    }

    std::cout << "\n  " << GREEN << BOLD
              << "  Estimated savings: $" << std::fixed << std::setprecision(2)
              << savings << "/mo  ($" << savings * 12.0 << "/yr)\n" << RESET;
}

void Dashboard::print_help() {
    hline_double(CYAN);
    std::cout << BOLD << CYAN
              << "  AUTO STORAGE TIERING SYSTEM — COMMANDS\n" << RESET;
    hline();

    struct Cmd { std::string key, desc; };
    std::vector<Cmd> cmds = {
        {"1",  "Run tiering cycle"},
        {"2",  "Tier summary"},
        {"3",  "Migration history"},
        {"4",  "Dry-run analysis"},
        {"5",  "List HOT files"},
        {"6",  "List WARM files"},
        {"7",  "List COLD files"},
        {"8",  "List ARCHIVE files"},
        {"f",  "Access a file (interactive)"},
        {"sc", "Scan real folder"},
        {"a",  "Add a file"},
        {"p",  "Pin / Unpin"},
        {"s",  "Savings report"},
        {"h",  "Show this help"},
        {"st", "System status"},
        {"q",  "Quit"},
    };
    for (auto& c : cmds) {
        std::cout << "  " << BOLD << CYAN << " [" << c.key << "] " << RESET
                  << c.desc << "\n";
    }
    hline();
}

void Dashboard::print_savings_report() {
    double actual = catalog_->total_monthly_cost();
    int64_t total_bytes = catalog_->total_bytes();
    double max_cost = (total_bytes / (1024.0 * 1024.0 * 1024.0))
                      * tier_cost_per_gb(Tier::HOT) * 30.0;
    double savings = max_cost - actual;
    double pct = max_cost > 0 ? savings / max_cost * 100 : 0;

    std::cout << "\n" << BOLD << "  COST & SAVINGS REPORT\n" << RESET;
    hline();
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  All-HOT baseline cost   :  $" << max_cost << " / month\n";
    std::cout << "  Actual tiered cost      :  $" << actual << " / month\n";
    std::cout << "  " << GREEN << BOLD
              << "  Monthly savings          :  $" << savings
              << " / month  (" << pct << "%)" << RESET << "\n";
    std::cout << "  " << GREEN << BOLD
              << "  Projected annual savings :  $" << savings * 12.0
              << " / year" << RESET << "\n";
    std::cout << "  Tiering cycles run      :  " << engine_->cycle_count() << "\n";
    std::cout << "  Total migrations done   :  " << engine_->total_migrations() << "\n";
    std::cout << "  Total bytes migrated    :  "
              << format_bytes(engine_->total_bytes_migrated()) << "\n";
    hline();
}

void Dashboard::print_system_status() {
    std::cout << "\n" << BOLD << "  SYSTEM STATUS\n" << RESET;
    hline();
    std::cout << "  Engine state      : " << engine_->state_name() << "\n";
    std::cout << "  Files managed     : " << catalog_->file_count() << "\n";
    std::cout << "  Total bytes       : " << format_bytes(catalog_->total_bytes()) << "\n";
    std::cout << "  Cycles run        : " << engine_->cycle_count() << "\n";
    std::cout << "  Total migrations  : " << engine_->total_migrations() << "\n";
    std::cout << "  Bytes migrated    : " << format_bytes(engine_->total_bytes_migrated()) << "\n";
    hline();
}

void Dashboard::print_engine_state() {
    std::cout << "\n  Engine: " << engine_->state_name()
              << "  Cycles: " << engine_->cycle_count()
              << "  Migrations: " << engine_->total_migrations()
              << "  Moved: " << format_bytes(engine_->total_bytes_migrated())
              << "\n";
}

void Dashboard::access_file_interactive() {
    auto files = catalog_->all_files();
    if (files.empty()) {
        std::cout << "\n  " << RED << "No files in catalog.\n" << RESET;
        return;
    }

    std::sort(files.begin(), files.end(),
        [](const FileRecord& a, const FileRecord& b) { return a.id < b.id; });

    const int PAGE = 20;
    int page = 0;
    int total = static_cast<int>(files.size());
    int pages = (total + PAGE - 1) / PAGE;

    while (true) {
        std::cout << "\n  " << BOLD
                  << pad_right("ID", 12) << pad_right("File", 38)
                  << pad_left("Size", 10) << "  "
                  << pad_right("Tier", 6) << pad_left("Idle(d)", 8)
                  << pad_left("Accesses", 10) << "\n" << RESET;
        std::cout << "  " << std::string(84, '-') << "\n";

        int lo = page * PAGE, hi = std::min(lo + PAGE, total);
        for (int i = lo; i < hi; ++i) {
            auto& f = files[i];
            std::string name = f.path;
            auto sep = name.find_last_of("/\\");
            if (sep != std::string::npos) name = name.substr(sep + 1);
            if (name.size() > 37) name = "..." + name.substr(name.size() - 34);

            std::cout << "  " << CYAN << pad_right(f.id, 12) << RESET
                      << pad_right(name, 38)
                      << pad_left(format_bytes(f.size_bytes), 10) << "  "
                      << tier_color(f.current_tier)
                      << pad_right(tier_name(f.current_tier), 6) << RESET
                      << std::fixed << std::setprecision(1)
                      << pad_left(std::to_string(static_cast<int>(f.idle_days())) + "d", 8)
                      << pad_left(std::to_string(f.access_count), 10) << "\n";
        }
        std::cout << "\n  Page " << page + 1 << "/" << pages
                  << "  [n]ext [p]rev  or enter File ID: ";

        std::string inp; std::cin >> inp;
        if (inp == "n" || inp == "N") { if (page < pages - 1) page++; continue; }
        if (inp == "p" || inp == "P") { if (page > 0) page--; continue; }
        if (inp == "q" || inp == "Q") return;

        auto file_opt = catalog_->get_file(inp);
        if (!file_opt.has_value()) {
            // Try partial match
            for (auto& fl : files) {
                if (fl.id == inp) {
                    file_opt = catalog_->get_file(fl.id);
                    break;
                }
            }
        }
        if (!file_opt.has_value()) {
            std::cout << "  " << RED << "Not found: " << inp << RESET << "\n";
            continue;
        }

        FileRecord f = file_opt.value();
        double score_before = policy_->score_file(f);
        Tier tier_before = f.current_tier;
        Tier rec_before = policy_->target_tier(f);

        std::cout << "\n  " << std::string(64, '-') << "\n";
        std::cout << "  " << BOLD << "Accessing: " << f.path << RESET << "\n";
        std::cout << "  " << std::string(64, '-') << "\n";
        std::cout << "  " << DIM << "BEFORE access:\n" << RESET;
        std::cout << "    Current tier : " << tier_color(f.current_tier) << BOLD
                  << tier_name(f.current_tier) << RESET << "\n";
        std::cout << "    Optimal tier : " << tier_color(rec_before)
                  << tier_name(rec_before) << RESET << "\n";
        std::cout << "    Access count : " << f.access_count << "\n";
        std::cout << "    Idle days    : " << std::fixed << std::setprecision(1)
                  << f.idle_days() << " d\n";
        std::cout << "    Score        : " << (score_before >= 0 ? GREEN : RED)
                  << (score_before >= 0 ? "+" : "")
                  << std::fixed << std::setprecision(1) << score_before << RESET << "\n";
        std::cout << "    " << DIM << policy_->explain_score(f) << RESET << "\n";

        // Record access
        catalog_->record_access(f.id);
        auto updated = catalog_->get_file(f.id);
        if (!updated.has_value()) { std::cout << "  Error refreshing record\n"; break; }

        f = updated.value();
        double score_after = policy_->score_file(f);
        Tier rec_after = policy_->target_tier(f);

        std::cout << "\n  " << GREEN << "  [*] File read successfully!\n" << RESET;
        std::cout << "\n  " << DIM << "AFTER access:\n" << RESET;
        std::cout << "    Current tier : " << tier_color(f.current_tier) << BOLD
                  << tier_name(f.current_tier) << RESET << "\n";
        std::cout << "    Optimal tier : " << tier_color(rec_after)
                  << tier_name(rec_after) << RESET << "\n";
        std::cout << "    Access count : " << GREEN << f.access_count << RESET << "\n";
        std::cout << "    Idle days    : " << GREEN << "0.0 d  (just accessed!)\n" << RESET;
        std::cout << "    Score        : " << (score_after >= 0 ? GREEN : RED)
                  << (score_after >= 0 ? "+" : "")
                  << std::fixed << std::setprecision(1) << score_after << RESET << "\n";

        std::cout << "\n  " << std::string(64, '-') << "\n";
        if (rec_after != tier_before) {
            std::cout << "  " << YELLOW << BOLD
                      << "  [!] Tier recommendation changed!\n" << RESET;
            std::cout << "      " << tier_color(tier_before) << tier_name(tier_before) << RESET
                      << "  ->  " << tier_color(rec_after) << tier_name(rec_after) << RESET
                      << "  (run [1] to apply)\n";
        } else {
            std::cout << "  " << GREEN
                      << "  [OK] File stays on " << tier_name(tier_before)
                      << " (optimal)\n" << RESET;
        }
        break;
    }
}

void Dashboard::add_file_interactive() {
    static int counter = 1000;
    std::cout << "\n  Add New File\n";
    std::cout << "  Path: ";
    std::string path; std::cin.ignore(); std::getline(std::cin, path);
    std::cout << "  Size in MB: ";
    double mb; std::cin >> mb;
    std::cout << "  Access count: ";
    int acc; std::cin >> acc;

    std::string ext;
    auto dot = path.rfind('.');
    if (dot != std::string::npos) ext = path.substr(dot);

    FileRecord f;
    f.id = "file_" + std::to_string(++counter);
    f.path = path;
    f.extension = ext;
    f.file_type = classify_extension(ext);
    f.size_bytes = static_cast<int64_t>(mb * 1024 * 1024);
    f.access_count = acc;
    f.write_count = 0;
    f.created_at = time(nullptr);
    f.last_accessed = time(nullptr);
    f.last_modified = time(nullptr);
    f.current_tier = Tier::HOT;
    f.target_tier = Tier::HOT;
    f.migrate_count = 0;
    f.is_pinned = false;
    f.is_critical = (f.file_type == FileType::DATABASE);
    f.score = 0.0;

    catalog_->add_file(f);
    std::cout << "  " << GREEN << "[OK] File added: " << f.id
              << "  (" << file_type_name(f.file_type)
              << ", starts on HOT)\n" << RESET;
}

void Dashboard::pin_file_interactive() {
    std::cout << "\n  Enter file ID to pin/unpin: ";
    std::string id; std::cin >> id;
    auto file_opt = catalog_->get_file(id);
    if (!file_opt.has_value()) {
        std::cout << RED << "  Not found.\n" << RESET;
        return;
    }
    auto f = file_opt.value();
    f.is_pinned = !f.is_pinned;
    catalog_->update_file(f);
    std::cout << "  " << GREEN << f.path << " is now "
              << (f.is_pinned ? "PINNED (always HOT)" : "UNPINNED (auto-tier on)")
              << RESET << "\n";
}


