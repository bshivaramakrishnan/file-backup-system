// Enterprise Communication Platform with Distributed Backup (ECPB)
// Main entry point

#include "common/types.h"
#include "common/logger.h"
#include "storage/database.h"
#include "storage/chunk_store.h"
#include "backup/orchestrator.h"
#include "restore/restore_engine.h"
#include "scheduler/job_scheduler.h"
#include "messaging/messaging.h"
#include "ui/terminal_ui.h"

#include <iostream>
#include <string>
#include <filesystem>
#include <cstring>

namespace fs = std::filesystem;

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [OPTIONS]\n"
              << "Options:\n"
              << "  --data-dir <path>   Data directory (default: ./ecpb_data)\n"
              << "  --log-level <N>     0=DEBUG, 1=INFO, 2=WARN, 3=ERROR (default: 1)\n"
              << "  --help              Show this help\n"
              << "\nNon-interactive mode:\n"
              << "  --backup <source> --name <name>   Run a backup\n"
              << "  --restore <job_id> --dest <path>  Restore a backup\n"
              << "  --list                            List all jobs\n"
              << "  --verify <job_id>                 Verify backup integrity\n"
              << "  --stats                           Show system stats\n";
}

int main(int argc, char* argv[]) {
    std::string data_dir = "./ecpb_data";
    int log_level = 1;

    // Non-interactive mode flags
    std::string backup_source, backup_name, restore_dest;
    int restore_id = -1, verify_id = -1;
    bool do_list = false, do_stats = false, non_interactive = false;

    // Parse args
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]); return 0;
        } else if (std::strcmp(argv[i], "--data-dir") == 0 && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--log-level") == 0 && i + 1 < argc) {
            log_level = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--backup") == 0 && i + 1 < argc) {
            backup_source = argv[++i]; non_interactive = true;
        } else if (std::strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            backup_name = argv[++i];
        } else if (std::strcmp(argv[i], "--restore") == 0 && i + 1 < argc) {
            restore_id = std::atoi(argv[++i]); non_interactive = true;
        } else if (std::strcmp(argv[i], "--dest") == 0 && i + 1 < argc) {
            restore_dest = argv[++i];
        } else if (std::strcmp(argv[i], "--verify") == 0 && i + 1 < argc) {
            verify_id = std::atoi(argv[++i]); non_interactive = true;
        } else if (std::strcmp(argv[i], "--list") == 0) {
            do_list = true; non_interactive = true;
        } else if (std::strcmp(argv[i], "--stats") == 0) {
            do_stats = true; non_interactive = true;
        } else {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            print_usage(argv[0]); return 1;
        }
    }

    // Setup logger
    if (log_level < 0 || log_level > 3) log_level = 1;
    ecpb::Logger::instance().set_level(static_cast<ecpb::LogLevel>(log_level));

    // Create data directory structure
    std::string db_path = data_dir + "/ecpb.db";
    std::string store_path = data_dir + "/store";
    std::string snapshot_path = data_dir + "/snapshots";

    try {
        fs::create_directories(data_dir);
        fs::create_directories(store_path);
        fs::create_directories(snapshot_path);
    } catch (const std::exception& e) {
        std::cerr << "Failed to create data directories: " << e.what() << "\n";
        return 1;
    }

    // Initialize core components
    ecpb::Database db;
    if (!db.open(db_path)) {
        std::cerr << "Failed to open database: " << db_path << "\n";
        return 1;
    }

    // Orchestrator creates its own ChunkStore internally at data_dir + "/storage"
    ecpb::BackupOrchestrator orchestrator(db, data_dir);
    ecpb::RestoreEngine restore_engine(db, orchestrator.chunk_store());
    ecpb::MessagingService messaging(db);

    LOG_INFO("ECPB initialized. Data dir: %s", data_dir.c_str());

    // Non-interactive mode
    if (non_interactive) {
        if (!backup_source.empty()) {
            if (backup_name.empty())
                backup_name = "backup_" + std::to_string(ecpb::now_epoch_ms());
            int job_id = orchestrator.submit_job(
                backup_source, backup_name,
                ecpb::JobPriority::NORMAL,
                ecpb::CompressionType::LZ4,
                true
            );
            if (job_id < 0) {
                std::cerr << "Failed to create backup job.\n"; return 1;
            }
            std::cout << "Backup job #" << job_id << " created. Running...\n";
            orchestrator.run_single_threaded();
            auto job = db.get_job(job_id);
            if (job && job->status == ecpb::JobStatus::COMPLETED) {
                std::cout << "Backup completed. Files: " << job->file_count
                          << ", Size: " << ecpb::format_bytes(job->total_bytes)
                          << ", Stored: " << ecpb::format_bytes(job->stored_bytes) << "\n";
                return 0;
            } else {
                std::cerr << "Backup failed.\n"; return 1;
            }
        }

        if (restore_id >= 0) {
            if (restore_dest.empty()) {
                std::cerr << "Missing --dest for restore.\n"; return 1;
            }
            auto result = restore_engine.restore_job(restore_id, restore_dest);
            if (result.success) {
                std::cout << "Restored " << result.files_restored << " files ("
                          << ecpb::format_bytes(result.bytes_restored) << ") to "
                          << restore_dest << "\n";
                return 0;
            } else {
                std::cerr << "Restore failed: " << result.error << "\n"; return 1;
            }
        }

        if (verify_id >= 0) {
            bool ok = restore_engine.verify_backup(verify_id);
            std::cout << "Backup #" << verify_id << ": "
                      << (ok ? "VERIFIED" : "FAILED") << "\n";
            return ok ? 0 : 1;
        }

        if (do_list) {
            auto jobs = db.get_all_jobs();
            for (auto& j : jobs) {
                std::cout << "#" << j.job_id << " " << j.backup_name
                          << " [" << ecpb::job_status_str(j.status) << "] "
                          << j.file_count << " files, "
                          << ecpb::format_bytes(j.total_bytes) << "\n";
            }
            return 0;
        }

        if (do_stats) {
            auto stats = db.get_stats();
            std::cout << "Jobs: " << stats.total_jobs
                      << " (completed: " << stats.completed_jobs
                      << ", failed: " << stats.failed_jobs << ")\n"
                      << "Chunks: " << stats.total_chunks << "\n"
                      << "Stored: " << ecpb::format_bytes(stats.total_stored_bytes) << "\n"
                      << "Dedup savings: " << ecpb::format_bytes(stats.total_dedup_savings) << "\n";
            return 0;
        }
    }

    // Interactive mode - launch terminal UI
    ecpb::TerminalUI ui(orchestrator, restore_engine, messaging);
    ui.run();

    LOG_INFO("ECPB shutdown.");
    return 0;
}
