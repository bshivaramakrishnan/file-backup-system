#pragma once

#include "common/types.h"
#include "common/logger.h"
#include <lz4.h>
#include <zstd.h>
#include <vector>
#include <cstring>

namespace ecpb {

class Compressor {
public:
    // Compress data using specified algorithm
    static std::vector<uint8_t> compress(const uint8_t* data, size_t len, CompressionType type) {
        switch (type) {
            case CompressionType::NONE: return std::vector<uint8_t>(data, data + len);
            case CompressionType::LZ4:  return compress_lz4(data, len);
            case CompressionType::ZSTD: return compress_zstd(data, len);
        }
        return std::vector<uint8_t>(data, data + len);
    }

    static std::vector<uint8_t> compress(const std::vector<uint8_t>& data, CompressionType type) {
        return compress(data.data(), data.size(), type);
    }

    // Decompress data. original_size must be known.
    static std::vector<uint8_t> decompress(const uint8_t* data, size_t len,
                                           size_t original_size, CompressionType type) {
        switch (type) {
            case CompressionType::NONE: return std::vector<uint8_t>(data, data + len);
            case CompressionType::LZ4:  return decompress_lz4(data, len, original_size);
            case CompressionType::ZSTD: return decompress_zstd(data, len, original_size);
        }
        return std::vector<uint8_t>(data, data + len);
    }

    static std::vector<uint8_t> decompress(const std::vector<uint8_t>& data,
                                           size_t original_size, CompressionType type) {
        return decompress(data.data(), data.size(), original_size, type);
    }

private:
    static std::vector<uint8_t> compress_lz4(const uint8_t* data, size_t len) {
        int max_dst = LZ4_compressBound(static_cast<int>(len));
        std::vector<uint8_t> output(max_dst);
        int compressed_size = LZ4_compress_default(
            reinterpret_cast<const char*>(data),
            reinterpret_cast<char*>(output.data()),
            static_cast<int>(len),
            max_dst
        );
        if (compressed_size <= 0) {
            LOG_ERR("LZ4 compression failed");
            return {};
        }
        output.resize(compressed_size);
        return output;
    }

    static std::vector<uint8_t> decompress_lz4(const uint8_t* data, size_t len, size_t original_size) {
        std::vector<uint8_t> output(original_size);
        int result = LZ4_decompress_safe(
            reinterpret_cast<const char*>(data),
            reinterpret_cast<char*>(output.data()),
            static_cast<int>(len),
            static_cast<int>(original_size)
        );
        if (result < 0) {
            LOG_ERR("LZ4 decompression failed");
            return {};
        }
        output.resize(result);
        return output;
    }

    static std::vector<uint8_t> compress_zstd(const uint8_t* data, size_t len) {
        size_t max_dst = ZSTD_compressBound(len);
        std::vector<uint8_t> output(max_dst);
        size_t compressed_size = ZSTD_compress(
            output.data(), max_dst,
            data, len,
            3  // compression level
        );
        if (ZSTD_isError(compressed_size)) {
            LOG_ERR("ZSTD compression failed: %s", ZSTD_getErrorName(compressed_size));
            return {};
        }
        output.resize(compressed_size);
        return output;
    }

    static std::vector<uint8_t> decompress_zstd(const uint8_t* data, size_t len, size_t original_size) {
        std::vector<uint8_t> output(original_size);
        size_t result = ZSTD_decompress(
            output.data(), original_size,
            data, len
        );
        if (ZSTD_isError(result)) {
            LOG_ERR("ZSTD decompression failed: %s", ZSTD_getErrorName(result));
            return {};
        }
        output.resize(result);
        return output;
    }
};

} // namespace ecpb
