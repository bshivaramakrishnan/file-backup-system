#pragma once

#include "common/types.h"
#include "common/logger.h"
#include "crypto/sha256.h"
#include "crypto/aes256.h"
#include "compression/compressor.h"
#include "storage/database.h"
#include "storage/rolling_checksum.h"
#include "datastructures/hash_map.h"
#include "datastructures/bplus_tree.h"

#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <cstring>

namespace ecpb {

class ChunkStore {
public:
    ChunkStore(Database& db, const std::string& storage_dir)
        : db_(db), storage_dir_(storage_dir) {
        // Create storage directory structure
        mkdir_p(storage_dir_);
        mkdir_p(storage_dir_ + "/chunks");
    }

    // Process and store a single file, returning its manifest
    FileManifest store_file(const std::string& file_path,
                            CompressionType comp, bool encrypt,
                            const AES256::Key& aes_key,
                            int job_id,
                            const std::string& relative_path = "") {
        FileManifest manifest;
        manifest.file_path = relative_path.empty() ? file_path : relative_path;
        manifest.file_name = basename_of(file_path);

        struct stat st;
        if (stat(file_path.c_str(), &st) != 0) {
            LOG_ERR("ChunkStore: cannot stat %s", file_path.c_str());
            return manifest;
        }
        manifest.file_size = static_cast<uint64_t>(st.st_size);
        manifest.modified_time = static_cast<uint64_t>(st.st_mtime);

        // Compute full file hash
        HashDigest file_digest = SHA256::hash_file(file_path);
        manifest.file_hash = SHA256::to_hex(file_digest);

        // Read and chunk the file
        std::ifstream file(file_path, std::ios::binary);
        if (!file.is_open()) {
            LOG_ERR("ChunkStore: cannot open %s", file_path.c_str());
            return manifest;
        }

        std::vector<uint8_t> buffer(CHUNK_SIZE);
        uint32_t chunk_idx = 0;
        uint64_t offset = 0;

        while (file) {
            file.read(reinterpret_cast<char*>(buffer.data()), CHUNK_SIZE);
            auto bytes_read = file.gcount();
            if (bytes_read <= 0) break;

            size_t chunk_size = static_cast<size_t>(bytes_read);
            std::vector<uint8_t> chunk_data(buffer.begin(), buffer.begin() + chunk_size);

            // Hash the chunk
            HashDigest chunk_digest = SHA256::hash(chunk_data.data(), chunk_size);
            HashHex chunk_hash = SHA256::to_hex(chunk_digest);

            ChunkInfo ci;
            ci.hash = chunk_hash;
            ci.offset = offset;
            ci.size = static_cast<uint32_t>(chunk_size);
            ci.chunk_index = chunk_idx;

            // Deduplication check
            if (db_.chunk_exists(chunk_hash.str())) {
                ci.deduplicated = true;
                LOG_DEBUG("Chunk %s deduplicated", chunk_hash.c_str());
            } else {
                ci.deduplicated = false;
                // Process: compress then encrypt
                std::vector<uint8_t> processed = chunk_data;

                // Compress
                if (comp != CompressionType::NONE) {
                    processed = Compressor::compress(processed, comp);
                    if (processed.empty()) {
                        processed = chunk_data;  // fallback to uncompressed
                    }
                }

                // Encrypt
                if (encrypt) {
                    processed = AES256::encrypt(processed, aes_key);
                    if (processed.empty()) {
                        LOG_ERR("ChunkStore: encryption failed for chunk %s", chunk_hash.c_str());
                        continue;
                    }
                }

                // Write to content-addressable storage
                std::string chunk_path = get_chunk_path(chunk_hash.str());
                mkdir_p(dirname_of(chunk_path));

                std::ofstream out(chunk_path, std::ios::binary);
                if (!out.is_open()) {
                    LOG_ERR("ChunkStore: cannot write chunk %s", chunk_path.c_str());
                    continue;
                }
                out.write(reinterpret_cast<const char*>(processed.data()), processed.size());
                out.close();

                // Store in database
                db_.store_chunk(chunk_hash.str(), chunk_path,
                               static_cast<uint32_t>(chunk_size),
                               static_cast<uint32_t>(processed.size()),
                               static_cast<int>(comp), encrypt);

                // Index in B+ tree
                chunk_index_.insert(chunk_hash.str(), chunk_path);

                // Track in dedup index
                dedup_index_.insert(chunk_hash.str(), true);
            }

            manifest.chunks.push_back(ci);
            offset += chunk_size;
            ++chunk_idx;
        }

        // Store manifest in DB
        db_.store_file_manifest(job_id, manifest);

        LOG_INFO("Stored file: %s (%s, %u chunks)",
                 manifest.file_name.c_str(),
                 format_bytes(manifest.file_size).c_str(),
                 chunk_idx);
        return manifest;
    }

    // Restore a file from its manifest
    bool restore_file(const FileManifest& manifest, const std::string& dest_path,
                      CompressionType comp, bool encrypted,
                      const AES256::Key& aes_key) {
        mkdir_p(dirname_of(dest_path));
        std::ofstream out(dest_path, std::ios::binary);
        if (!out.is_open()) {
            LOG_ERR("ChunkStore: cannot create restore target %s", dest_path.c_str());
            return false;
        }

        for (auto& chunk : manifest.chunks) {
            // Find chunk storage path
            std::string chunk_path;
            auto cached = chunk_index_.find(chunk.hash.str());
            if (cached) {
                chunk_path = *cached;
            } else {
                chunk_path = db_.get_chunk_path(chunk.hash.str());
            }

            if (chunk_path.empty()) {
                LOG_ERR("ChunkStore: chunk %s not found", chunk.hash.c_str());
                return false;
            }

            // Read chunk data
            std::ifstream in(chunk_path, std::ios::binary);
            if (!in.is_open()) {
                LOG_ERR("ChunkStore: cannot read chunk file %s", chunk_path.c_str());
                return false;
            }
            std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                                       std::istreambuf_iterator<char>());
            in.close();

            // Decrypt
            if (encrypted) {
                data = AES256::decrypt(data, aes_key);
                if (data.empty()) {
                    LOG_ERR("ChunkStore: decryption failed for chunk %s", chunk.hash.c_str());
                    return false;
                }
            }

            // Decompress
            if (comp != CompressionType::NONE) {
                data = Compressor::decompress(data, chunk.size, comp);
                if (data.empty()) {
                    LOG_ERR("ChunkStore: decompression failed for chunk %s", chunk.hash.c_str());
                    return false;
                }
            }

            // Verify integrity
            HashDigest digest = SHA256::hash(data.data(), data.size());
            HashHex computed_hash = SHA256::to_hex(digest);
            if (computed_hash != chunk.hash) {
                LOG_ERR("ChunkStore: integrity check failed for chunk %s", chunk.hash.c_str());
                return false;
            }

            out.write(reinterpret_cast<const char*>(data.data()), data.size());
        }

        out.close();

        // Verify restored file hash
        HashDigest restored_digest = SHA256::hash_file(dest_path);
        HashHex restored_hash = SHA256::to_hex(restored_digest);
        if (restored_hash != manifest.file_hash) {
            LOG_ERR("ChunkStore: file hash mismatch after restore for %s", dest_path.c_str());
            return false;
        }

        LOG_INFO("Restored: %s (%s)", dest_path.c_str(), format_bytes(manifest.file_size).c_str());
        return true;
    }

    // Get dedup stats
    size_t dedup_index_size() const { return dedup_index_.size(); }
    size_t chunk_index_size() const { return chunk_index_.size(); }

private:
    Database& db_;
    std::string storage_dir_;
    HashMap<std::string, bool> dedup_index_;
    BPlusTree<std::string, std::string> chunk_index_;

    // Content-addressable path: chunks/ab/cd/abcdef....
    std::string get_chunk_path(const std::string& hash_hex) {
        return storage_dir_ + "/chunks/" +
               hash_hex.substr(0, 2) + "/" +
               hash_hex.substr(2, 2) + "/" +
               hash_hex;
    }

    static std::string basename_of(const std::string& path) {
        auto pos = path.rfind('/');
        return (pos != std::string::npos) ? path.substr(pos + 1) : path;
    }

    static std::string dirname_of(const std::string& path) {
        auto pos = path.rfind('/');
        return (pos != std::string::npos) ? path.substr(0, pos) : ".";
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
