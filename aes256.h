#pragma once

#include "common/types.h"
#include "common/logger.h"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <vector>
#include <cstring>
#include <string>
#include <array>

namespace ecpb {

class AES256 {
public:
    using Key = std::array<uint8_t, AES_KEY_LEN>;
    using IV  = std::array<uint8_t, AES_IV_LEN>;

    static Key generate_key() {
        Key key{};
        if (RAND_bytes(key.data(), AES_KEY_LEN) != 1) {
            LOG_ERR("AES256: failed to generate random key");
        }
        return key;
    }

    static IV generate_iv() {
        IV iv{};
        if (RAND_bytes(iv.data(), AES_IV_LEN) != 1) {
            LOG_ERR("AES256: failed to generate random IV");
        }
        return iv;
    }

    // Encrypt data. Returns IV prepended to ciphertext.
    static std::vector<uint8_t> encrypt(const uint8_t* plaintext, size_t len, const Key& key) {
        IV iv = generate_iv();
        std::vector<uint8_t> output;
        // Prepend IV
        output.insert(output.end(), iv.begin(), iv.end());

        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx) {
            LOG_ERR("AES256: failed to create cipher context");
            return {};
        }

        if (EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr, key.data(), iv.data()) != 1) {
            LOG_ERR("AES256: EncryptInit failed");
            EVP_CIPHER_CTX_free(ctx);
            return {};
        }

        // Output buffer: plaintext + block size for padding
        size_t out_alloc = len + AES_BLOCK_SIZE;
        size_t offset = output.size();
        output.resize(offset + out_alloc);

        int out_len1 = 0;
        if (EVP_EncryptUpdate(ctx, output.data() + offset, &out_len1,
                              plaintext, static_cast<int>(len)) != 1) {
            LOG_ERR("AES256: EncryptUpdate failed");
            EVP_CIPHER_CTX_free(ctx);
            return {};
        }

        int out_len2 = 0;
        if (EVP_EncryptFinal_ex(ctx, output.data() + offset + out_len1, &out_len2) != 1) {
            LOG_ERR("AES256: EncryptFinal failed");
            EVP_CIPHER_CTX_free(ctx);
            return {};
        }

        output.resize(offset + out_len1 + out_len2);
        EVP_CIPHER_CTX_free(ctx);
        return output;
    }

    static std::vector<uint8_t> encrypt(const std::vector<uint8_t>& plaintext, const Key& key) {
        return encrypt(plaintext.data(), plaintext.size(), key);
    }

    // Decrypt data. Expects IV prepended to ciphertext.
    static std::vector<uint8_t> decrypt(const uint8_t* data, size_t len, const Key& key) {
        if (len < AES_IV_LEN) {
            LOG_ERR("AES256: data too short for IV");
            return {};
        }

        IV iv{};
        std::memcpy(iv.data(), data, AES_IV_LEN);
        const uint8_t* ciphertext = data + AES_IV_LEN;
        size_t cipher_len = len - AES_IV_LEN;

        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx) {
            LOG_ERR("AES256: failed to create cipher context");
            return {};
        }

        if (EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr, key.data(), iv.data()) != 1) {
            LOG_ERR("AES256: DecryptInit failed");
            EVP_CIPHER_CTX_free(ctx);
            return {};
        }

        std::vector<uint8_t> output(cipher_len + AES_BLOCK_SIZE);
        int out_len1 = 0;
        if (EVP_DecryptUpdate(ctx, output.data(), &out_len1,
                              ciphertext, static_cast<int>(cipher_len)) != 1) {
            LOG_ERR("AES256: DecryptUpdate failed");
            EVP_CIPHER_CTX_free(ctx);
            return {};
        }

        int out_len2 = 0;
        if (EVP_DecryptFinal_ex(ctx, output.data() + out_len1, &out_len2) != 1) {
            LOG_ERR("AES256: DecryptFinal failed (bad key or corrupt data)");
            EVP_CIPHER_CTX_free(ctx);
            return {};
        }

        output.resize(out_len1 + out_len2);
        EVP_CIPHER_CTX_free(ctx);
        return output;
    }

    static std::vector<uint8_t> decrypt(const std::vector<uint8_t>& data, const Key& key) {
        return decrypt(data.data(), data.size(), key);
    }

    // Key <-> hex string conversions
    static std::string key_to_hex(const Key& key) {
        char hex[AES_KEY_LEN * 2 + 1] = {};
        for (size_t i = 0; i < AES_KEY_LEN; ++i) {
            std::snprintf(hex + i * 2, 3, "%02x", key[i]);
        }
        return std::string(hex);
    }

    static Key key_from_hex(const std::string& hex) {
        Key key{};
        if (hex.size() < AES_KEY_LEN * 2) return key;
        for (size_t i = 0; i < AES_KEY_LEN; ++i) {
            unsigned int byte;
            std::sscanf(hex.c_str() + i * 2, "%02x", &byte);
            key[i] = static_cast<uint8_t>(byte);
        }
        return key;
    }
};

} // namespace ecpb
