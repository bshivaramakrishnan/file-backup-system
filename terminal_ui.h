#pragma once

#include "common/types.h"
#include "common/logger.h"
#include "backup/orchestrator.h"
#include "restore/restore_engine.h"
#include "messaging/messaging.h"

#include <iostream>
#include <string>
#include <limits>
#include <iomanip>

namespace ecpb {

class TerminalUI {
public:
    TerminalUI(BackupOrchestrator& orch, RestoreEngine& restore, MessagingService& msg)
        : orch_(orch), restore_(restore), msg_(msg) {}

    void run() {
        std::cout << "\n";
        print_banner();
        while (true) {
            print_menu();
            int choice = read_int("Select option: ");
            switch (choice) {
                case 1: do_backup(); break;
                case 2: do_restore(); break;
                case 3: list_jobs(); break;
                case 4: verify_backup(); break;
                case 5: show_stats(); break;
                case 6: do_messaging(); break;
                case 7: set_log_level(); break;
                case 0:
                    std::cout << "Shutting down...\n";
                    return;
                default:
                    std::cout << "Invalid option.\n";
            }
        }
    }

private:
    BackupOrchestrator& orch_;
    RestoreEngine& restore_;
    MessagingService& msg_;

    void print_banner() {
        std::cout << "========================================\n"
                  << "  Enterprise Backup System (ECPB)\n"
                  << "  C++ | POSIX | SQLite | AES-256\n"
                  << "========================================\n";
    }

    void print_menu() {
        std::cout << "\n--- Main Menu ---\n"
                  << "  1) Create Backup\n"
                  << "  2) Restore Backup\n"
                  << "  3) List Jobs\n"
                  << "  4) Verify Backup\n"
                  << "  5) System Stats\n"
                  << "  6) Messaging\n"
                  << "  7) Set Log Level\n"
                  << "  0) Exit\n";
    }

    void do_backup() {
        std::string source = read_line("Source path: ");
        if (source.empty()) { std::cout << "Cancelled.\n"; return; }

        std::string name = read_line("Backup name: ");
        if (name.empty()) name = "backup_" + std::to_string(now_epoch_ms());

        std::cout << "Priority (0=LOW, 1=NORMAL, 2=HIGH, 3=URGENT) [1]: ";
        int pri = read_int_default(1);
        if (pri < 0 || pri > 3) pri = 1;

        std::cout << "Compression (0=NONE, 1=LZ4, 2=ZSTD) [1]: ";
        int comp = read_int_default(1);
        if (comp < 0 || comp > 2) comp = 1;

        std::cout << "Encrypt? (1=yes, 0=no) [1]: ";
        int enc = read_int_default(1);

        int job_id = orch_.submit_job(
            source, name,
            static_cast<JobPriority>(pri),
            static_cast<CompressionType>(comp),
            enc != 0
        );

        if (job_id < 0) {
            std::cout << "Failed to create backup job.\n";
            return;
        }

        std::cout << "Backup job #" << job_id << " created.\n";
        std::cout << "Running backup...\n";

        orch_.run_single_threaded();

        auto job = orch_.database().get_job(job_id);
        if (job && job->status == JobStatus::COMPLETED) {
            std::cout << "Backup completed!\n";
            print_job_details(*job);
        } else if (job) {
            std::cout << "Backup " << job_status_str(job->status) << "\n";
            if (!job->error_message.empty())
                std::cout << "Error: " << job->error_message << "\n";
        }
    }

    void do_restore() {
        auto restorable = restore_.list_restorable();
        if (restorable.empty()) {
            std::cout << "No completed backups available to restore.\n";
            return;
        }

        std::cout << "\nAvailable backups:\n";
        print_separator();
        std::cout << std::setw(5) << "ID" << " | "
                  << std::setw(20) << "Name" << " | "
                  << std::setw(10) << "Files" << " | "
                  << std::setw(12) << "Size" << " | "
                  << "Date\n";
        print_separator();
        for (auto& j : restorable) {
            std::cout << std::setw(5) << j.job_id << " | "
                      << std::setw(20) << j.backup_name.substr(0, 20) << " | "
                      << std::setw(10) << j.file_count << " | "
                      << std::setw(12) << format_bytes(j.total_bytes) << " | "
                      << epoch_to_string(j.completed_at) << "\n";
        }
        print_separator();

        int job_id = read_int("Enter backup ID to restore: ");
        std::string dest = read_line("Destination path: ");
        if (dest.empty()) { std::cout << "Cancelled.\n"; return; }

        std::cout << "Restoring...\n";
        auto result = restore_.restore_job(job_id, dest);

        if (result.success) {
            std::cout << "Restore completed!\n"
                      << "  Files: " << result.files_restored << "\n"
                      << "  Size:  " << format_bytes(result.bytes_restored) << "\n"
                      << "  Location: " << dest << "\n";
            for (auto& f : result.restored_files) {
                std::cout << "    - " << f << "\n";
            }
        } else {
            std::cout << "Restore failed: " << result.error << "\n";
        }
    }

    void list_jobs() {
        auto jobs = orch_.database().get_all_jobs();
        if (jobs.empty()) {
            std::cout << "No jobs found.\n";
            return;
        }
        std::cout << "\n";
        print_separator();
        std::cout << std::setw(5) << "ID" << " | "
                  << std::setw(20) << "Name" << " | "
                  << std::setw(10) << "Status" << " | "
                  << std::setw(8) << "Priority" << " | "
                  << std::setw(10) << "Files" << " | "
                  << std::setw(12) << "Total" << " | "
                  << std::setw(12) << "Stored" << " | "
                  << std::setw(12) << "Dedup" << "\n";
        print_separator();
        for (auto& j : jobs) {
            std::cout << std::setw(5) << j.job_id << " | "
                      << std::setw(20) << j.backup_name.substr(0, 20) << " | "
                      << std::setw(10) << job_status_str(j.status) << " | "
                      << std::setw(8) << job_priority_str(j.priority) << " | "
                      << std::setw(10) << j.file_count << " | "
                      << std::setw(12) << format_bytes(j.total_bytes) << " | "
                      << std::setw(12) << format_bytes(j.stored_bytes) << " | "
                      << std::setw(12) << format_bytes(j.dedup_savings) << "\n";
        }
        print_separator();
    }

    void verify_backup() {
        int job_id = read_int("Enter backup ID to verify: ");
        std::cout << "Verifying...\n";
        if (restore_.verify_backup(job_id)) {
            std::cout << "Backup #" << job_id << " integrity verified.\n";
        } else {
            std::cout << "Backup #" << job_id << " integrity check FAILED.\n";
        }
    }

    void show_stats() {
        auto stats = orch_.database().get_stats();
        std::cout << "\n--- System Statistics ---\n"
                  << "  Total Jobs:       " << stats.total_jobs << "\n"
                  << "  Completed:        " << stats.completed_jobs << "\n"
                  << "  Failed:           " << stats.failed_jobs << "\n"
                  << "  Total Chunks:     " << stats.total_chunks << "\n"
                  << "  Stored Data:      " << format_bytes(stats.total_stored_bytes) << "\n"
                  << "  Dedup Savings:    " << format_bytes(stats.total_dedup_savings) << "\n"
                  << "  Backed Up Files:  " << stats.total_files << "\n"
                  << "  Dedup Index:      " << orch_.chunk_store().dedup_index_size() << " entries\n"
                  << "  Chunk Index:      " << orch_.chunk_store().chunk_index_size() << " entries\n";
    }

    void do_messaging() {
        std::cout << "\n--- Messaging ---\n"
                  << "  1) Send Message\n"
                  << "  2) View Messages\n"
                  << "  3) Share File\n"
                  << "  0) Back\n";
        int choice = read_int("Select: ");
        switch (choice) {
            case 1: {
                std::string ch = read_line("Channel: ");
                if (ch.empty()) break;
                msg_.create_channel(ch);
                std::string sender = read_line("Your name: ");
                std::string content = read_line("Message: ");
                if (msg_.send_message(ch, sender, content))
                    std::cout << "Message sent.\n";
                else
                    std::cout << "Failed.\n";
                break;
            }
            case 2: {
                std::string ch = read_line("Channel: ");
                auto msgs = msg_.get_messages(ch);
                if (msgs.empty()) {
                    std::cout << "No messages.\n";
                } else {
                    for (auto& m : msgs) {
                        std::cout << "[" << epoch_to_string(m.created_at) << "] "
                                  << m.sender << ": " << m.content << "\n";
                    }
                }
                break;
            }
            case 3: {
                std::string ch = read_line("Channel: ");
                msg_.create_channel(ch);
                std::string sender = read_line("Your name: ");
                std::string file = read_line("File path: ");
                int jid = read_int("Related backup job ID (0=none): ");
                if (msg_.share_file(ch, sender, file, jid))
                    std::cout << "File shared.\n";
                else
                    std::cout << "Failed.\n";
                break;
            }
            default: break;
        }
    }

    void set_log_level() {
        std::cout << "Log level (0=DEBUG, 1=INFO, 2=WARN, 3=ERROR) [1]: ";
        int lvl = read_int_default(1);
        if (lvl < 0 || lvl > 3) lvl = 1;
        Logger::instance().set_level(static_cast<LogLevel>(lvl));
        std::cout << "Log level set.\n";
    }

    void print_job_details(const BackupJob& j) {
        std::cout << "  Name:        " << j.backup_name << "\n"
                  << "  Source:      " << j.source_path << "\n"
                  << "  Files:       " << j.file_count << "\n"
                  << "  Total:       " << format_bytes(j.total_bytes) << "\n"
                  << "  Stored:      " << format_bytes(j.stored_bytes) << "\n"
                  << "  Dedup:       " << format_bytes(j.dedup_savings) << "\n"
                  << "  Compression: " << compression_str(j.compression) << "\n"
                  << "  Encrypted:   " << (j.encrypt ? "Yes" : "No") << "\n"
                  << "  Duration:    ";
        if (j.started_at > 0 && j.completed_at > 0) {
            uint64_t ms = j.completed_at - j.started_at;
            std::cout << ms << " ms\n";
        } else {
            std::cout << "N/A\n";
        }
    }

    static void print_separator() {
        std::cout << std::string(110, '-') << "\n";
    }

    static std::string read_line(const char* prompt) {
        std::cout << prompt;
        std::string line;
        std::getline(std::cin, line);
        return line;
    }

    static int read_int(const char* prompt) {
        std::cout << prompt;
        std::string line;
        std::getline(std::cin, line);
        try { return std::stoi(line); }
        catch (...) { return -1; }
    }

    static int read_int_default(int def) {
        std::string line;
        std::getline(std::cin, line);
        if (line.empty()) return def;
        try { return std::stoi(line); }
        catch (...) { return def; }
    }
};

} // namespace ecpb
