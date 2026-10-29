// Unit tests for MD5 and CRC-32 against known vectors.
#include "gputxrx/checksum.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace gputxrx;

static int failures = 0;

static void expect_hex(const uint8_t* d, size_t n, const char* want, const char* what) {
    std::string got;
    static const char* hx = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        got += hx[d[i] >> 4];
        got += hx[d[i] & 0xF];
    }
    bool ok = got == want;
    std::printf("[%s] %s (got %s want %s)\n", ok ? "PASS" : "FAIL", what, got.c_str(), want);
    if (!ok) ++failures;
}

int main() {
    // MD5 RFC 1321 test vectors.
    {
        uint8_t d[16];
        Md5::digest(nullptr, 0, d);
        expect_hex(d, 16, "d41d8cd98f00b204e9800998ecf8427e", "md5(empty)");
    }
    {
        const char* s = "a";
        uint8_t d[16];
        Md5::digest((const uint8_t*)s, 1, d);
        expect_hex(d, 16, "0cc175b9c0f1b6a831c399e269772661", "md5(\"a\")");
    }
    {
        const char* s = "abc";
        uint8_t d[16];
        Md5::digest((const uint8_t*)s, 3, d);
        expect_hex(d, 16, "900150983cd24fb0d6963f7d28e17f72", "md5(\"abc\")");
    }
    {
        const char* s = "message digest";
        uint8_t d[16];
        Md5::digest((const uint8_t*)s, 14, d);
        expect_hex(d, 16, "f96b697d7cb7938d525a2f31aaf161d0", "md5(\"message digest\")");
    }
    {
        const char* s = "abcdefghijklmnopqrstuvwxyz";
        uint8_t d[16];
        Md5::digest((const uint8_t*)s, 26, d);
        expect_hex(d, 16, "c3fcd3d76192e4007dfb496cca67e13b", "md5(a-z)");
    }
    {
        std::string s;
        for (int i = 0; i < 8; ++i) s += "1234567890"; // "1234567890" x8
        uint8_t d[16];
        Md5::digest((const uint8_t*)s.data(), s.size(), d);
        expect_hex(d, 16, "57edf4a22be3c955ac49da2e2107b67a", "md5(1234567890 x8)");
    }
    // Streaming == one-shot for a multi-block input.
    {
        std::vector<uint8_t> big((1u << 20) + 123);
        for (size_t i = 0; i < big.size(); ++i) big[i] = uint8_t(i * 131);
        uint8_t one[16], stream[16];
        Md5::digest(big.data(), big.size(), one);
        Md5 h;
        for (size_t off = 0; off < big.size(); off += 7777) {
            size_t n = std::min<size_t>(7777, big.size() - off);
            h.update(big.data() + off, n);
        }
        h.final(stream);
        bool ok = std::memcmp(one, stream, 16) == 0;
        std::printf("[%s] md5 streaming equals one-shot\n", ok ? "PASS" : "FAIL");
        if (!ok) ++failures;
    }
    // CRC-32 (IEEE, zlib-compatible).
    {
        uint32_t c = crc32(nullptr, 0);
        bool ok = c == 0x00000000u;
        std::printf("[%s] crc32(empty) == 0\n", ok ? "PASS" : "FAIL");
        if (!ok) ++failures;
    }
    {
        const char* s = "123456789";
        uint32_t c = crc32((const uint8_t*)s, 9);
        bool ok = c == 0xCBF43926u;
        std::printf("[%s] crc32(\"123456789\") == 0xCBF43926\n", ok ? "PASS" : "FAIL");
        if (!ok) ++failures;
    }
    {
        const char* s = "The quick brown fox jumps over the lazy dog";
        uint32_t c1 = crc32((const uint8_t*)s, 43);
        uint32_t c2 = 0;
        for (const char* p = s; *p; ++p) c2 = crc32((const uint8_t*)p, 1, c2);
        bool ok = c1 == c2;
        std::printf("[%s] crc32 streaming equals one-shot\n", ok ? "PASS" : "FAIL");
        if (!ok) ++failures;
    }

    std::printf("=== test_checksums %s (%d failures) ===\n", failures ? "FAILED" : "PASSED",
                failures);
    return failures ? 1 : 0;
}
