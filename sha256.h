#pragma once

#include "common/types.h"
#include "common/logger.h"
#include <openssl/evp.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>

namespace ecpb {

class SHA256 {
public:
    // Hash a byte buffer
    static HashDigest hash(const uint8_t* data, size_t len) {
        HashDigest digest{};
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        if (!ctx) {
            LOG_ERR("SHA256: failed to create EVP context");
            return digest;
        }
        unsigned int digest_len = 0;
        if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1 ||
            EVP_DigestUpdate(ctx, data, len) != 1 ||
            EVP_DigestFinal_ex(ctx, digest.data(), &digest_len) != 1) {
            LOG_ERR("SHA256: digest computation failed");
        }
        EVP_MD_CTX_free(ctx);
        return digest;
    }

    // Hash a string
    static HashDigest hash(const std::string& data) {
        return hash(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    }

    // Hash a vector
    static HashDigest hash(const std::vector<uint8_t>& data) {
        return hash(data.data(), data.size());
    }

    // Streaming hash for large data
    class Stream {
    public:
        Stream() : ctx_(EVP_MD_CTX_new()), valid_(false) {
            if (ctx_ && EVP_DigestInit_ex(ctx_, EVP_sha256(), nullptr) == 1) {
                valid_ = true;
            }
        }
        ~Stream() { if (ctx_) EVP_MD_CTX_free(ctx_); }

        Stream(const Stream&) = delete;
        Stream& operator=(const Stream&) = delete;

        bool update(const uint8_t* data, size_t len) {
            if (!valid_) return false;
            return EVP_DigestUpdate(ctx_, data, len) == 1;
        }

        HashDigest finalize() {
            HashDigest digest{};
            if (valid_) {
                unsigned int len = 0;
                EVP_DigestFinal_ex(ctx_, digest.data(), &len);
                valid_ = false;
            }
            return digest;
        }

    private:
        EVP_MD_CTX* ctx_;
        bool valid_;
    };

    // Hash a file
    static HashDigest hash_file(const std::string& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            LOG_ERR("SHA256: cannot open file %s", path.c_str());
            return HashDigest{};
        }
        Stream stream;
        uint8_t buf[65536];
        while (file.read(reinterpret_cast<char*>(buf), sizeof(buf)) || file.gcount() > 0) {
            stream.update(buf, static_cast<size_t>(file.gcount()));
            if (file.eof()) break;
        }
        return stream.finalize();
    }

    // Convert digest to hex string
    static HashHex to_hex(const HashDigest& digest) {
        HashHex hex;
        for (size_t i = 0; i < SHA256_BIN_LEN; ++i) {
            std::snprintf(hex.data + i * 2, 3, "%02x", digest[i]);
        }
        return hex;
    }

    // Convert hex back to digest
    static HashDigest from_hex(const HashHex& hex) {
        HashDigest digest{};
        for (size_t i = 0; i < SHA256_BIN_LEN; ++i) {
            unsigned int byte;
            std::sscanf(hex.data + i * 2, "%02x", &byte);
            digest[i] = static_cast<uint8_t>(byte);
        }
        return digest;
    }

    // Convenience: hash and return hex
    static HashHex hash_hex(const uint8_t* data, size_t len) {
        return to_hex(hash(data, len));
    }

    static HashHex hash_hex(const std::string& data) {
        return to_hex(hash(data));
    }
};

} // namespace ecpb
