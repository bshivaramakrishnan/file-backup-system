#pragma once

#include "common/types.h"
#include <cstdint>
#include <cstddef>

namespace ecpb {

// Adler32-style rolling checksum for rsync-like block matching
class RollingChecksum {
public:
    static constexpr uint32_t MOD = 65521;

    RollingChecksum() : a_(1), b_(0), window_size_(0), count_(0) {}

    void reset() { a_ = 1; b_ = 0; window_size_ = 0; count_ = 0; }

    // Update with a single byte
    void update(uint8_t byte) {
        a_ = (a_ + byte) % MOD;
        b_ = (b_ + a_) % MOD;
        ++count_;
    }

    // Bulk update
    void update(const uint8_t* data, size_t len) {
        for (size_t i = 0; i < len; ++i) {
            a_ = (a_ + data[i]) % MOD;
            b_ = (b_ + a_) % MOD;
        }
        count_ += len;
    }

    // Roll window: remove old_byte, add new_byte
    void roll(uint8_t old_byte, uint8_t new_byte, size_t window_len) {
        a_ = (a_ - old_byte + new_byte + MOD) % MOD;
        b_ = (b_ - window_len * old_byte + a_ - 1 + MOD * 2) % MOD;
    }

    uint32_t digest() const {
        return (b_ << 16) | a_;
    }

    // Compute full checksum of a block
    static uint32_t compute(const uint8_t* data, size_t len) {
        RollingChecksum rc;
        rc.update(data, len);
        return rc.digest();
    }

    // Check if two blocks are likely the same (weak check)
    static bool weak_match(uint32_t checksum1, uint32_t checksum2) {
        return checksum1 == checksum2;
    }

private:
    uint32_t a_, b_;
    size_t window_size_;
    size_t count_;
};

} // namespace ecpb
