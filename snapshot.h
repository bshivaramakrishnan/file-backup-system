#pragma once

#include "common/types.h"
#include "common/logger.h"
#include "storage/database.h"

#include <string>
#include <vector>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <cstring>
#include <fstream>
#include <cerrno>

namespace ecpb {

class SnapshotManager {
public:
    explicit SnapshotManager(Database& db, const std::string& snapshot_base_dir)
        : db_(db), base_dir_(snapshot_base_dir) {
        mkdir_p(base_dir_);
    }

    // Create a snapshot of a source directory via hardlinks (CoW-style)
    // This gives us a consistent view without blocking the source
    SnapshotInfo create_snapshot(int job_id, const std::string& source_path) {
        SnapshotInfo info;
        info.job_id = job_id;
        info.created_at = now_epoch_ms();

        std::string snap_dir = base_dir_ + "/snap_" + std::to_string(job_id) +
                               "_" + std::to_string(info.created_at);
        mkdir_p(snap_dir);
        info.snapshot_path = snap_dir;

        struct stat st;
        if (stat(source_path.c_str(), &st) != 0) {
            LOG_ERR("Snapshot: source path does not exist: %s", source_path.c_str());
            info.is_consistent = false;
            return info;
        }

        bool ok;
        if (S_ISDIR(st.st_mode)) {
            ok = snapshot_directory(source_path, snap_dir);
        } else {
            // Single file - just copy it
            ok = copy_file(source_path, snap_dir + "/" + basename_of(source_path));
        }

        info.is_consistent = ok;
        if (ok) {
            LOG_INFO("Snapshot created: %s", snap_dir.c_str());
        } else {
            LOG_ERR("Snapshot creation failed for job %d", job_id);
        }
        return info;
    }

    // Remove a snapshot after backup is complete
    bool remove_snapshot(const SnapshotInfo& info) {
        if (info.snapshot_path.empty()) return false;
        return remove_recursive(info.snapshot_path);
    }

    // List all files in a snapshot
    std::vector<std::string> list_files(const SnapshotInfo& info) {
        std::vector<std::string> files;
        if (info.snapshot_path.empty()) return files;
        list_files_recursive(info.snapshot_path, files);
        return files;
    }

private:
    Database& db_;
    std::string base_dir_;

    bool snapshot_directory(const std::string& src, const std::string& dst) {
        DIR* dir = opendir(src.c_str());
        if (!dir) {
            LOG_ERR("Snapshot: cannot open directory %s: %s", src.c_str(), strerror(errno));
            return false;
        }

        bool ok = true;
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name = entry->d_name;
            if (name == "." || name == "..") continue;

            std::string src_path = src + "/" + name;
            std::string dst_path = dst + "/" + name;

            struct stat st;
            if (lstat(src_path.c_str(), &st) != 0) continue;

            if (S_ISDIR(st.st_mode)) {
                mkdir_p(dst_path);
                if (!snapshot_directory(src_path, dst_path)) ok = false;
            } else if (S_ISREG(st.st_mode)) {
                // Try hardlink first (true CoW), fallback to copy
                if (link(src_path.c_str(), dst_path.c_str()) != 0) {
                    if (!copy_file(src_path, dst_path)) ok = false;
                }
            }
        }
        closedir(dir);
        return ok;
    }

    static bool copy_file(const std::string& src, const std::string& dst) {
        std::ifstream in(src, std::ios::binary);
        std::ofstream out(dst, std::ios::binary);
        if (!in.is_open() || !out.is_open()) return false;
        out << in.rdbuf();
        return out.good();
    }

    static bool remove_recursive(const std::string& path) {
        struct stat st;
        if (lstat(path.c_str(), &st) != 0) return true;

        if (S_ISDIR(st.st_mode)) {
            DIR* dir = opendir(path.c_str());
            if (!dir) return false;
            struct dirent* entry;
            while ((entry = readdir(dir)) != nullptr) {
                std::string name = entry->d_name;
                if (name == "." || name == "..") continue;
                remove_recursive(path + "/" + name);
            }
            closedir(dir);
            return rmdir(path.c_str()) == 0;
        }
        return unlink(path.c_str()) == 0;
    }

    static void list_files_recursive(const std::string& path, std::vector<std::string>& files) {
        DIR* dir = opendir(path.c_str());
        if (!dir) return;
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name = entry->d_name;
            if (name == "." || name == "..") continue;
            std::string full = path + "/" + name;
            struct stat st;
            if (lstat(full.c_str(), &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) {
                list_files_recursive(full, files);
            } else if (S_ISREG(st.st_mode)) {
                files.push_back(full);
            }
        }
        closedir(dir);
    }

    static std::string basename_of(const std::string& path) {
        auto pos = path.rfind('/');
        return (pos != std::string::npos) ? path.substr(pos + 1) : path;
    }

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
