#include "gputxrx/payload.hpp"

#include <algorithm>
#include <cstring>
#include <set>

namespace gputxrx::payload {

namespace {

[[noreturn]] void fail(const std::string& msg) { throw PayloadError(msg); }

uint64_t checked_add(uint64_t a, uint64_t b, const char* what) {
    if (a > kMaxTotalSize - b) fail(std::string("integer overflow in ") + what);
    return a + b;
}

uint64_t checked_mul(uint64_t a, uint64_t b, const char* what) {
    if (a != 0 && b != 0 && a > kMaxTotalSize / b)
        fail(std::string("integer overflow in ") + what);
    return a * b;
}

bool is_reserved_device_name(const std::string& component) {
    static const char* names[] = {"CON",  "PRN", "AUX", "NUL", "COM1", "COM2", "COM3",
                                  "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
                                  "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6",
                                  "LPT7", "LPT8", "LPT9"};
    size_t dot = component.find('.');
    std::string base = dot == std::string::npos ? component : component.substr(0, dot);
    if (base.empty()) return false;
    for (char& c : base)
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    for (const char* n : names)
        if (base == n) return true;
    return false;
}

} // namespace

void put_u32(uint8_t* p, uint32_t v) noexcept {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16);
    p[3] = uint8_t(v >> 24);
}

void put_u64(uint8_t* p, uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (8 * i));
}

uint32_t get_u32(const uint8_t* p) noexcept {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
}

uint64_t get_u64(const uint8_t* p) noexcept {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

void validate_payload_path(const std::string& p) {
    if (p.empty()) fail("unsafe payload path: empty path");
    if (p.size() > kMaxPathBytes)
        fail("unsafe payload path: longer than " + std::to_string(kMaxPathBytes) + " bytes: " + p);
    if (p.find('\0') != std::string::npos) fail("unsafe payload path: contains NUL byte");
    if (p.front() == '/' || p.front() == '\\')
        fail("unsafe payload path: absolute path: " + p);
    if (p.find('\\') != std::string::npos)
        fail("unsafe payload path: backslash separators are not allowed: " + p);
    if (p.size() >= 2 && p[1] == ':')
        fail("unsafe payload path: drive-qualified path: " + p);
    if (p.back() == '/') fail("unsafe payload path: trailing separator: " + p);
    if (p.find("//") != std::string::npos) fail("unsafe payload path: empty component: " + p);

    size_t start = 0;
    while (start <= p.size()) {
        size_t end = p.find('/', start);
        std::string comp = p.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (comp == "." || comp == "..")
            fail("unsafe payload path: '" + comp + "' component: " + p);
        if (!comp.empty()) {
            for (char c : comp)
                if (unsigned char(c) < 0x20)
                    fail("unsafe payload path: control character: " + p);
            if (comp.back() == '.' || comp.back() == ' ')
                fail("unsafe payload path: component ends with '.' or ' ': " + p);
            if (is_reserved_device_name(comp))
                fail("unsafe payload path: reserved device name: " + p);
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
}

Layout plan_layout(const std::vector<LayoutInput>& files) {
    Layout l;
    l.manifest_offset = kHeaderSize;

    if (files.size() > kMaxFileCount)
        fail("too many files: " + std::to_string(files.size()) + " > " +
             std::to_string(kMaxFileCount));

    uint64_t path_blob_size = 0;
    for (const auto& f : files) {
        validate_payload_path(f.path);
        if (f.path.size() > kMaxPathBytes) fail("path too long: " + f.path);
        path_blob_size = checked_add(path_blob_size, f.path.size(), "path blob size");
    }
    if (path_blob_size > kMaxManifestSize) fail("path blob exceeds maximum manifest size");

    uint64_t entries_bytes =
        checked_mul(files.size(), kManifestEntrySize, "manifest entries size");
    l.manifest_size = checked_add(entries_bytes, path_blob_size, "manifest size");
    if (l.manifest_size > kMaxManifestSize) fail("manifest exceeds maximum size");

    l.path_blob_offset = checked_add(l.manifest_offset, entries_bytes, "path blob offset");
    l.path_blob_size = path_blob_size;
    l.data_region_offset = checked_add(l.manifest_offset, l.manifest_size, "data region offset");

    l.data_offsets.resize(files.size());
    uint64_t off = l.data_region_offset;
    for (size_t i = 0; i < files.size(); ++i) {
        if (files[i].size > kMaxFileSize)
            fail("file too large: " + files[i].path + " (" + std::to_string(files[i].size) +
                 " bytes)");
        l.data_offsets[i] = off;
        off = checked_add(off, files[i].size, "data region size");
    }
    l.data_region_size = off - l.data_region_offset;
    l.total_size = checked_add(l.data_region_offset, l.data_region_size, "total size");
    if (l.total_size > kMaxTotalSize) fail("payload exceeds hard format cap");
    return l;
}

void write_header(uint8_t* dest, const Header& h) {
    std::memset(dest, 0, kHeaderSize);
    std::memcpy(dest, kMagic, 4);
    put_u32(dest + 4, kFormatVersion);
    put_u32(dest + 8, kHeaderSize);
    put_u32(dest + 12, kManifestEntrySize);
    std::memcpy(dest + 16, h.transfer_id.data(), 16);
    put_u64(dest + 32, h.total_size);
    put_u64(dest + 40, h.manifest_offset);
    put_u64(dest + 48, h.manifest_size);
    put_u64(dest + 56, h.file_count);
    put_u64(dest + 64, h.data_region_offset);
    put_u64(dest + 72, h.data_region_size);
    put_u32(dest + kCrcFieldOffset, h.payload_crc32);
    put_u32(dest + 84, 0); // flags
}

Header parse_header(const uint8_t* buf, uint64_t available) {
    if (available < kHeaderSize)
        fail("truncated payload: header is " + std::to_string(available) + " of " +
             std::to_string(kHeaderSize) + " bytes");
    if (std::memcmp(buf, kMagic, 4) != 0) fail("bad magic: not a gputxrx payload");
    uint32_t version = get_u32(buf + 4);
    if (version != kFormatVersion)
        fail("unsupported format version " + std::to_string(version));
    if (get_u32(buf + 8) != kHeaderSize) fail("bad header size field");
    if (get_u32(buf + 12) != kManifestEntrySize) fail("bad manifest entry size field");

    Header h;
    std::memcpy(h.transfer_id.data(), buf + 16, 16);
    h.total_size = get_u64(buf + 32);
    h.manifest_offset = get_u64(buf + 40);
    h.manifest_size = get_u64(buf + 48);
    h.file_count = get_u64(buf + 56);
    h.data_region_offset = get_u64(buf + 64);
    h.data_region_size = get_u64(buf + 72);
    h.payload_crc32 = get_u32(buf + kCrcFieldOffset);

    if (h.manifest_offset != kHeaderSize) fail("bad manifest offset");
    if (h.manifest_size > kMaxManifestSize) fail("manifest size exceeds maximum");
    if (h.file_count > kMaxFileCount) fail("file count exceeds maximum");
    uint64_t end_manifest = checked_add(h.manifest_offset, h.manifest_size, "manifest end");
    if (end_manifest != h.data_region_offset) fail("data region does not follow manifest");
    uint64_t end_total =
        checked_add(h.data_region_offset, h.data_region_size, "payload end");
    if (end_total != h.total_size) fail("total_size does not cover data region");
    for (uint32_t i = 88; i < kHeaderSize; ++i)
        if (buf[i] != 0) fail("nonzero reserved header byte");
    return h;
}

void write_manifest_entry(uint8_t* dest, uint64_t manifest_offset, uint32_t index,
                          uint64_t path_offset, const Entry& e) {
    if (e.path.size() > kMaxPathBytes) fail("path too long: " + e.path);
    uint8_t* p = dest + manifest_offset + uint64_t(index) * kManifestEntrySize;
    std::memset(p, 0, kManifestEntrySize);
    put_u64(p, path_offset);
    put_u32(p + 8, uint32_t(e.path.size()));
    put_u32(p + 12, 0);
    put_u64(p + 16, e.size);
    put_u64(p + 24, e.data_offset);
    std::memcpy(p + 32, e.md5.data(), 16);
}

std::vector<Entry> parse_manifest(const uint8_t* buf, const Header& h) {
    const uint64_t entries_bytes = h.file_count * kManifestEntrySize; // bounded by parse_header
    const uint64_t path_blob_offset = h.manifest_offset + entries_bytes;
    if (h.manifest_size < entries_bytes) fail("manifest smaller than its entries table");
    const uint64_t path_blob_size = h.manifest_size - entries_bytes;
    if (path_blob_size > kMaxManifestSize) fail("path blob exceeds maximum");

    std::vector<Entry> entries;
    entries.reserve(size_t(h.file_count));

    for (uint64_t i = 0; i < h.file_count; ++i) {
        const uint8_t* p = buf + h.manifest_offset + i * kManifestEntrySize;
        Entry e;
        uint64_t path_offset = get_u64(p);
        uint32_t path_len = get_u32(p + 8);
        uint32_t reserved = get_u32(p + 12);
        e.size = get_u64(p + 16);
        e.data_offset = get_u64(p + 24);
        std::memcpy(e.md5.data(), p + 32, 16);

        if (reserved != 0) fail("nonzero reserved manifest field in entry " + std::to_string(i));
        if (path_len > kMaxPathBytes) fail("path too long in entry " + std::to_string(i));
        if (path_offset > path_blob_size || path_len > path_blob_size - path_offset)
            fail("path out of manifest bounds in entry " + std::to_string(i));
        e.path.assign(reinterpret_cast<const char*>(buf + path_blob_offset + path_offset),
                      path_len);
        if (e.size > kMaxFileSize) fail("file size exceeds maximum in entry " + std::to_string(i));

        entries.push_back(std::move(e));
    }

    // Path safety and duplicates.
    std::set<std::string> seen;
    for (auto& e : entries) {
        validate_payload_path(e.path);
        if (!seen.insert(e.path).second) fail("duplicate path in manifest: " + e.path);
    }

    // Bounds and overlap of file data regions (requires sorted-by-offset
    // contiguity: the format is always written contiguous).
    std::vector<const Entry*> by_offset;
    by_offset.reserve(entries.size());
    for (const auto& e : entries) by_offset.push_back(&e);
    std::sort(by_offset.begin(), by_offset.end(),
              [](const Entry* a, const Entry* b) { return a->data_offset < b->data_offset; });

    uint64_t cursor = h.data_region_offset;
    for (const Entry* e : by_offset) {
        if (e->data_offset != cursor)
            fail("file data region is not contiguous at: " + e->path);
        cursor = checked_add(cursor, e->size, "file data end");
        if (cursor > h.data_region_offset + h.data_region_size)
            fail("file data region out of bounds: " + e->path);
    }
    if (cursor != h.data_region_offset + h.data_region_size)
        fail("data region size does not match file sizes");
    return entries;
}

} // namespace gputxrx::payload
