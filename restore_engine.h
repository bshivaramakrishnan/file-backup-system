#pragma once

#include "common/types.h"
#include "common/logger.h"
#include "storage/database.h"
#include "storage/chunk_store.h"
#include "crypto/aes256.h"

#include <string>
#include <vector>
#include <sys/stat.h>

namespace ecpb {

class RestoreEngine {
public:
    RestoreEngine(Database& db, ChunkStore& store)
        : db_(db), store_(store) {}

    struct RestoreResult {
        bool success = false;
        int files_restored = 0;
        uint64_t bytes_restored = 0;
        std::string error;
        std::vector<std::string> restored_files;
    };

    // Restore all files from a backup job
    RestoreResult restore_job(int job_id, const std::string& dest_path) {
        RestoreResult result;

        // Verify job exists and is completed
        auto job = db_.get_job(job_id);
        if (!job) {
            result.error = "Job not found: " + std::to_string(job_id);
            LOG_ERR("Restore: %s", result.error.c_str());
            return result;
        }
        if (job->status != JobStatus::COMPLETED) {
            result.error = "Job " + std::to_string(job_id) + " is not completed (status: " +
                           job_status_str(job->status) + ")";
            LOG_ERR("Restore: %s", result.error.c_str());
            return result;
        }

        // Get AES key if encrypted
        AES256::Key aes_key{};
        if (job->encrypt) {
            std::string key_hex = db_.get_encryption_key(job_id);
            if (key_hex.empty()) {
                result.error = "Encryption key not found for job " + std::to_string(job_id);
                LOG_ERR("Restore: %s", result.error.c_str());
                return result;
            }
            aes_key = AES256::key_from_hex(key_hex);
        }

        // Get all file manifests for this job
        auto manifests = db_.get_file_manifests(job_id);
        if (manifests.empty()) {
            result.error = "No files found in backup job " + std::to_string(job_id);
            LOG_WARN("Restore: %s", result.error.c_str());
            result.success = true;  // technically success, just no files
            return result;
        }

        LOG_INFO("Restore: restoring %zu files from job %d to %s",
                 manifests.size(), job_id, dest_path.c_str());

        mkdir_p(dest_path);

        for (auto& manifest : manifests) {
            // file_path is stored as relative path (e.g., "subdir/nested.txt")
            std::string target = dest_path + "/" + manifest.file_path;

            // Ensure parent directory exists
            auto slash_pos = target.rfind('/');
            if (slash_pos != std::string::npos) {
                mkdir_p(target.substr(0, slash_pos));
            }

            bool ok = store_.restore_file(manifest, target,
                                          job->compression, job->encrypt, aes_key);
            if (!ok) {
                LOG_ERR("Restore: failed to restore %s", manifest.file_path.c_str());
                result.error = "Failed to restore: " + manifest.file_name;
                // Continue with other files
            } else {
                result.files_restored++;
                result.bytes_restored += manifest.file_size;
                result.restored_files.push_back(target);
            }
        }

        result.success = (result.files_restored > 0);
        LOG_INFO("Restore complete: %d files, %s",
                 result.files_restored, format_bytes(result.bytes_restored).c_str());
        return result;
    }

    // List all restorable backups
    std::vector<BackupJob> list_restorable() {
        auto all_jobs = db_.get_all_jobs();
        std::vector<BackupJob> restorable;
        for (auto& j : all_jobs) {
            if (j.status == JobStatus::COMPLETED) {
                restorable.push_back(j);
            }
        }
        return restorable;
    }

    // Verify backup integrity without restoring
    bool verify_backup(int job_id) {
        auto job = db_.get_job(job_id);
        if (!job || job->status != JobStatus::COMPLETED) return false;

        auto manifests = db_.get_file_manifests(job_id);
        for (auto& manifest : manifests) {
            for (auto& chunk : manifest.chunks) {
                auto meta = db_.get_chunk_meta(chunk.hash.str());
                if (!meta) {
                    LOG_ERR("Verify: chunk %s not found in database", chunk.hash.c_str());
                    return false;
                }
                // Check chunk file exists
                struct stat st;
                if (stat(meta->storage_path.c_str(), &st) != 0) {
                    LOG_ERR("Verify: chunk file missing: %s", meta->storage_path.c_str());
                    return false;
                }
            }
        }
        LOG_INFO("Verify: backup job %d integrity OK", job_id);
        return true;
    }

private:
    Database& db_;
    ChunkStore& store_;

    static void mkdir_p(const std::string& path) {
        std::string tmp;
        for (size_t i = 0; i < path.size(); ++i) {
            tmp += path[i];
            if (path[i] == '/' || i == path.size() - 1) {
                mkdir(tmp.c_str(), 0755);
            }
        }
    }
};

} // namespace ecpb
