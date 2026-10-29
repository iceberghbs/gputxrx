// Unit tests for the payload format: layout planning, header/manifest
// round-trip, tamper detection, unsafe paths, overlap/overflow rejection.
#include "gputxrx/payload.hpp"

#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

#include "gputxrx/checksum.hpp"

using namespace gputxrx::payload;
using gputxrx::crc32;

static int failures = 0;

static void expect(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

static void expect_throws(const char* what, const std::function<void()>& fn) {
    try {
        fn();
        expect(false, what);
    } catch (const PayloadError&) {
        expect(true, what);
    } catch (const std::exception& e) {
        std::printf("       unexpected exception: %s\n", e.what());
        expect(false, what);
    }
}

static void expect_ok(const char* what, const std::function<void()>& fn) {
    try {
        fn();
        expect(true, what);
    } catch (const std::exception& e) {
        std::printf("       unexpected exception: %s\n", e.what());
        expect(false, what);
    }
}

int main() {
    // Layout + serialize + parse round-trip.
    std::vector<LayoutInput> files = {
        {"a.txt", 11},
        {".git/config", 0},
        {"dir/empty.bin", 0},
        {"nested/deep/file.dat", 1024},
        {"unicode/日本語.txt", 5},
    };
    Layout l = plan_layout(files);
    expect(l.total_size == l.data_region_offset + 11 + 0 + 0 + 1024 + 5, "layout total size");
    expect(l.manifest_offset == kHeaderSize, "manifest follows header");

    std::vector<uint8_t> buf(size_t(l.total_size), 0);
    Header h;
    for (int i = 0; i < 16; ++i) h.transfer_id[i] = uint8_t(i * 7);
    h.total_size = l.total_size;
    h.manifest_offset = l.manifest_offset;
    h.manifest_size = l.manifest_size;
    h.file_count = files.size();
    h.data_region_offset = l.data_region_offset;
    h.data_region_size = l.data_region_size;

    std::vector<Entry> entries;
    uint64_t path_off = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        Entry e;
        e.path = files[i].path;
        e.size = files[i].size;
        e.data_offset = l.data_offsets[i];
        for (int j = 0; j < 16; ++j) e.md5[j] = uint8_t(i * 31 + j);
        write_manifest_entry(buf.data(), l.manifest_offset, uint32_t(i), path_off, e);
        std::memcpy(buf.data() + l.path_blob_offset + path_off, e.path.data(), e.path.size());
        path_off += e.path.size();
        entries.push_back(e);
    }
    h.payload_crc32 = 0;
    write_header(buf.data(), h);
    // Mirror the transmitter: crc over the final buffer with only the crc
    // field zeroed, then patch the field.
    {
        std::vector<uint8_t> zeroed = buf;
        std::memset(zeroed.data() + kCrcFieldOffset, 0, 4);
        h.payload_crc32 = crc32(zeroed.data(), zeroed.size());
        put_u32(zeroed.data() + kCrcFieldOffset, h.payload_crc32);
        write_header(zeroed.data(), h);
        std::memcpy(buf.data(), zeroed.data(), kHeaderSize);
    }

    Header parsed = parse_header(buf.data(), buf.size());
    expect(parsed.total_size == h.total_size && parsed.file_count == h.file_count &&
               parsed.payload_crc32 == h.payload_crc32,
           "header round-trip");
    {
        // The receiver-side CRC recomputation: zero only the crc field.
        std::vector<uint8_t> zeroed = buf;
        std::memset(zeroed.data() + kCrcFieldOffset, 0, 4);
        expect(parsed.payload_crc32 == crc32(zeroed.data(), zeroed.size()),
               "crc covers whole buffer (crc field zeroed)");
    }

    // Tamper: flip one data byte -> crc no longer matches.
    {
        std::vector<uint8_t> tampered = buf;
        tampered[size_t(l.data_region_offset) + 3] ^= 0xFF;
        Header ph = parse_header(tampered.data(), tampered.size());
        expect(ph.payload_crc32 != crc32(tampered.data(), tampered.size()),
               "crc detects data tampering");
    }

    auto parsed_entries = parse_manifest(buf.data(), parsed);
    expect(parsed_entries.size() == entries.size(), "manifest entry count round-trip");
    bool all_same = true;
    for (size_t i = 0; i < entries.size(); ++i)
        if (parsed_entries[i].path != entries[i].path || parsed_entries[i].size != entries[i].size ||
            parsed_entries[i].data_offset != entries[i].data_offset ||
            std::memcmp(parsed_entries[i].md5.data(), entries[i].md5.data(), 16) != 0)
            all_same = false;
    expect(all_same, "manifest entries round-trip (path/size/offset/md5)");

    // Unsafe paths.
    expect_throws("reject absolute path", [] { validate_payload_path("/etc/passwd"); });
    expect_throws("reject drive-qualified path", [] { validate_payload_path("C:/x"); });
    expect_throws("reject parent traversal", [] { validate_payload_path("a/../../b"); });
    expect_throws("reject dot component", [] { validate_payload_path("./a"); });
    expect_throws("reject backslash", [] { validate_payload_path("a\\b"); });
    expect_throws("reject empty path", [] { validate_payload_path(""); });
    expect_throws("reject trailing slash", [] { validate_payload_path("a/"); });
    expect_throws("reject empty component", [] { validate_payload_path("a//b"); });
    expect_throws("reject reserved device name", [] { validate_payload_path("NUL"); });
    expect_throws("reject trailing dot component", [] { validate_payload_path("a./b"); });
    expect_ok("accept dotfile path", [] { validate_payload_path(".gitignore"); });
    expect_ok("accept normal path", [] { validate_payload_path("a/b.c/d"); });

    // Malformed manifests built by hand.
    {
        // Out-of-bounds path offset.
        std::vector<uint8_t> bad = buf;
        put_u64(bad.data() + kHeaderSize, 0xFFFFFFFFFFFFull); // entry 0 path_offset
        expect_throws("reject out-of-bounds manifest path offset",
                      [&] { parse_manifest(bad.data(), parsed); });
    }
    {
        // Duplicate paths.
        std::vector<uint8_t> bad = buf;
        // Copy entry 1's fields over entry 0, then fix path blob: point both
        // entries at the same path text.
        uint8_t* e0 = bad.data() + kHeaderSize;
        uint8_t* e1 = bad.data() + kHeaderSize + kManifestEntrySize;
        put_u64(e0, get_u64(e1));       // same path_offset as entry 1
        put_u32(e0 + 8, get_u32(e1 + 8)); // same path length
        expect_throws("reject duplicate paths", [&] { parse_manifest(bad.data(), parsed); });
    }
    {
        // Non-contiguous / overlapping data regions: shrink entry 1's size and
        // move entry 2's offset to overlap entry 1's region.
        std::vector<uint8_t> bad = buf;
        uint8_t* e2 = bad.data() + kHeaderSize + 2 * kManifestEntrySize;
        put_u64(e2 + 24, get_u64(e2 + 24) - 1); // overlap previous region
        expect_throws("reject non-contiguous data regions",
                      [&] { parse_manifest(bad.data(), parsed); });
    }
    {
        // Truncated buffer.
        expect_throws("reject truncated header",
                      [&] { parse_header(buf.data(), 32); });
    }
    {
        // Wrong magic.
        std::vector<uint8_t> bad = buf;
        bad[0] = 'X';
        expect_throws("reject bad magic", [&] { parse_header(bad.data(), bad.size()); });
    }
    {
        // Reserved header byte set.
        std::vector<uint8_t> bad = buf;
        bad[100] = 1;
        expect_throws("reject nonzero reserved header bytes",
                      [&] { parse_header(bad.data(), bad.size()); });
    }

    std::printf("=== test_payload %s (%d failures) ===\n", failures ? "FAILED" : "PASSED",
                failures);
    return failures ? 1 : 0;
}
