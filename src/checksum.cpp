#include "gputxrx/checksum.hpp"

namespace gputxrx {

namespace {

inline uint32_t rotl(uint32_t x, uint32_t n) noexcept {
    return (x << n) | (x >> (32 - n));
}

const uint32_t* md5_k_table() noexcept {
    static const uint32_t k[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
        0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
        0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
        0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
        0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
        0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
        0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
        0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
    };
    return k;
}

const uint32_t* md5_shift_table() noexcept {
    static const uint32_t s[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
    };
    return s;
}

void md5_process(uint32_t state[4], const uint8_t block[64]) noexcept {
    const uint32_t* k = md5_k_table();
    const uint32_t* s = md5_shift_table();

    uint32_t m[16];
    for (int i = 0; i < 16; ++i) {
        m[i] = uint32_t(block[i * 4]) | (uint32_t(block[i * 4 + 1]) << 8) |
               (uint32_t(block[i * 4 + 2]) << 16) | (uint32_t(block[i * 4 + 3]) << 24);
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    for (int i = 0; i < 64; ++i) {
        uint32_t f;
        int g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        uint32_t tmp = d;
        d = c;
        c = b;
        uint32_t sum = a + f + k[i] + m[g];
        b = b + rotl(sum, s[i]);
        a = tmp;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

} // namespace

Md5::Md5() noexcept : state_{0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u},
                      bit_count_(0), buffer_{}, buffer_len_(0) {}

void Md5::update(const uint8_t* data, size_t len) noexcept {
    bit_count_ += static_cast<uint64_t>(len) * 8;
    while (len > 0) {
        size_t take = 64 - buffer_len_;
        if (take > len) take = len;
        for (size_t i = 0; i < take; ++i) buffer_[buffer_len_ + i] = data[i];
        buffer_len_ += take;
        data += take;
        len -= take;
        if (buffer_len_ == 64) {
            md5_process(state_, buffer_);
            buffer_len_ = 0;
        }
    }
}

void Md5::final(uint8_t out[kDigestSize]) noexcept {
    const uint64_t bits = bit_count_;
    static const uint8_t pad = 0x80;
    update(&pad, 1);
    const uint8_t zero = 0;
    while (buffer_len_ != 56) update(&zero, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; ++i) lenb[i] = uint8_t(bits >> (8 * i));
    // bit_count_ already included padding bits; restore by writing length manually.
    update(lenb, 8);
    (void)bits;
    // Note: update() above modified bit_count_, but we re-derive from saved bits
    // only through lenb, and state is already final after processing length block.
    for (int i = 0; i < 4; ++i) {
        out[i * 4] = uint8_t(state_[i]);
        out[i * 4 + 1] = uint8_t(state_[i] >> 8);
        out[i * 4 + 2] = uint8_t(state_[i] >> 16);
        out[i * 4 + 3] = uint8_t(state_[i] >> 24);
    }
}

void Md5::digest(const uint8_t* data, size_t len, uint8_t out[kDigestSize]) noexcept {
    Md5 h;
    h.update(data, len);
    h.final(out);
}

uint32_t crc32(const uint8_t* data, size_t len, uint32_t seed) noexcept {
    static uint32_t table[256];
    static bool init = [] {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        return true;
    }();
    (void)init;

    uint32_t c = seed ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i)
        c = table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

} // namespace gputxrx
