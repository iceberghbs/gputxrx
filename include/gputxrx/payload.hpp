#pragma once
// Serialized payload format.
//
// One contiguous buffer: [header][manifest entries table][path blob][file data].
//
// Byte order:      little-endian everywhere.
// String encoding: UTF-8, no NUL terminator inside the buffer ('/' separators).
// Integer widths:  unsigned 32/64-bit only.
// Alignment:       none required; all fields are byte-packed at fixed offsets.
//
// Header (192 bytes):
//   0   magic "GPXR"
//   4   format_version        u32 (1)
//   8   header_size           u32 (192)
//   12  manifest_entry_size   u32 (56)
//   16  transfer_id           16 bytes (random)
//   32  total_size            u64 (entire serialized buffer)
//   40  manifest_offset       u64 (== header_size; table + path blob)
//   48  manifest_size         u64 (entries table + path blob bytes)
//   56  file_count            u64
//   64  data_region_offset    u64 (== manifest_offset + manifest_size)
//   72  data_region_size      u64
//   80  payload_crc32         u32 (CRC-32/IEEE over the whole buffer with
//                                 bytes [80,84) treated as zero)
//   84  flags                 u32 (0)
//   88  reserved              104 bytes, must be zero
//
// Manifest entry (56 bytes each, back to back starting at manifest_offset):
//   0   path_offset   u64 (relative to the start of the path blob)
//   8   path_len      u32 (bytes, UTF-8)
//   12  reserved      u32 (0)
//   16  file_size     u64
//   24  data_offset   u64 (absolute offset in the serialized buffer)
//   32  md5           16 bytes
//
// Path blob: all paths concatenated, starting at manifest_offset +
// file_count * manifest_entry_size; total length == manifest_size -
// file_count * manifest_entry_size.
//
// Validation is strict on the receive side: unknown version, wrong sizes,
// out-of-bounds offsets, overlapping file regions, duplicate paths, unsafe
// paths, and any nonzero reserved bytes are rejected. MD5 (per file) and the
// whole-payload CRC-32 guard against accidental corruption, not adversaries.

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace gputxrx::payload {

class PayloadError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

inline constexpr char kMagic[4] = {'G', 'P', 'X', 'R'};
inline constexpr uint32_t kFormatVersion = 1;
inline constexpr uint32_t kHeaderSize = 192;
inline constexpr uint32_t kManifestEntrySize = 56;
inline constexpr uint32_t kCrcFieldOffset = 80;
inline constexpr uint64_t kMaxFileCount = 1000000;
inline constexpr uint32_t kMaxPathBytes = 4096;
inline constexpr uint64_t kMaxManifestSize = 512ull << 20;
inline constexpr uint64_t kMaxFileSize = 64ull << 30;    // per-file hard cap
inline constexpr uint64_t kMaxTotalSize = 1ull << 40;    // whole-payload hard cap

struct Header {
    std::array<uint8_t, 16> transfer_id{};
    uint64_t total_size = 0;
    uint64_t manifest_offset = 0;
    uint64_t manifest_size = 0;
    uint64_t file_count = 0;
    uint64_t data_region_offset = 0;
    uint64_t data_region_size = 0;
    uint32_t payload_crc32 = 0;
};

struct Entry {
    std::string path; // UTF-8, '/' separators
    uint64_t size = 0;
    uint64_t data_offset = 0;
    std::array<uint8_t, 16> md5{};
};

struct Layout {
    uint64_t total_size = 0;
    uint64_t manifest_offset = 0;
    uint64_t manifest_size = 0;
    uint64_t path_blob_offset = 0;
    uint64_t path_blob_size = 0;
    uint64_t data_region_offset = 0;
    uint64_t data_region_size = 0;
    std::vector<uint64_t> data_offsets; // per input file, absolute
};

struct LayoutInput {
    std::string path; // UTF-8
    uint64_t size = 0;
};

// Throws PayloadError if the path is not a safe relative path.
void validate_payload_path(const std::string& utf8);

// Computes the serialized layout with checked arithmetic. Throws PayloadError
// on overflow, oversized paths, or limit violations.
Layout plan_layout(const std::vector<LayoutInput>& files);

// Serialization helpers. `dest` must have room for the region being written.
void write_header(uint8_t* dest, const Header& h);
Header parse_header(const uint8_t* buf, uint64_t available); // throws PayloadError

// Writes entry i of `entries` into the entries table at manifest_offset,
// including its absolute path_offset within the path blob.
void write_manifest_entry(uint8_t* dest, uint64_t manifest_offset, uint32_t index,
                          uint64_t path_offset, const Entry& e);

// Parses and bounds-checks all manifest entries. `buf` points at the start of
// the serialized buffer; the full header region must be readable. Throws
// PayloadError on malformed entries, unsafe/duplicate paths, out-of-bounds or
// overlapping data regions.
std::vector<Entry> parse_manifest(const uint8_t* buf, const Header& h);

// Raw little-endian helpers (exposed for tests).
void put_u32(uint8_t* p, uint32_t v) noexcept;
void put_u64(uint8_t* p, uint64_t v) noexcept;
uint32_t get_u32(const uint8_t* p) noexcept;
uint64_t get_u64(const uint8_t* p) noexcept;

} // namespace gputxrx::payload
