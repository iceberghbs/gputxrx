#pragma once
// Modular checksum interface.
//
// MD5 is used for detecting accidental corruption of individual files, and a
// CRC-32 (IEEE) covers the whole serialized payload. Neither is suitable for
// adversarial authenticity. A SHA-256 implementation can be added later by
// implementing the same shape as gputxrx::Md5 and extending the format version.

#include <array>
#include <cstddef>
#include <cstdint>

namespace gputxrx {

class Md5 {
public:
    static constexpr size_t kDigestSize = 16;

    Md5() noexcept;
    void update(const uint8_t* data, size_t len) noexcept;
    void final(uint8_t out[kDigestSize]) noexcept;

    static void digest(const uint8_t* data, size_t len, uint8_t out[kDigestSize]) noexcept;

private:
    uint32_t state_[4];
    uint64_t bit_count_;
    uint8_t buffer_[64];
    size_t buffer_len_;
};

// IEEE CRC-32 (same polynomial as zlib), seed 0 for a fresh stream.
uint32_t crc32(const uint8_t* data, size_t len, uint32_t seed = 0) noexcept;

} // namespace gputxrx
