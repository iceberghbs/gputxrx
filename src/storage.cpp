#include "gputxrx/storage.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <set>

#include "gputxrx/checksum.hpp"

namespace gputxrx::storage {

namespace {

[[noreturn]] void fail(const std::string& msg) { throw StorageError(msg); }

std::string gle_message(const char* api, DWORD gle) {
    return std::string(api) + " failed (GetLastError " + std::to_string(gle) + ")";
}

bool is_reparse_point(const std::filesystem::path& p) {
    DWORD attrs = GetFileAttributesW(p.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT);
}

} // namespace

std::string path_to_utf8(const std::filesystem::path& p) {
    std::wstring w = p.wstring();
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) fail("WideCharToMultiByte failed converting path");
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::filesystem::path utf8_to_path(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) fail("MultiByteToWideChar failed converting payload path");
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return std::filesystem::path(w);
}

std::vector<FileRecord> collect_files(const std::filesystem::path& root) {
    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec))
        fail("input path is not an existing directory: " + path_to_utf8(root));
    if (is_reparse_point(root))
        fail("input directory itself is a symlink/junction, which is not supported: " +
             path_to_utf8(root));

    std::vector<FileRecord> out;
    for (auto it = std::filesystem::recursive_directory_iterator(root);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) fail("filesystem iteration failed: " + ec.message());
        const auto& entry = *it;

        std::error_code ec2;
        auto sym = entry.symlink_status(ec2);
        if (ec2) fail("cannot stat entry: " + path_to_utf8(entry.path()) + ": " + ec2.message());
        if (std::filesystem::is_symlink(sym) || is_reparse_point(entry.path()))
            fail("symbolic links and reparse points are not supported: " +
                 path_to_utf8(entry.path()));

        if (entry.is_directory(ec2)) {
            if (ec2) fail("cannot inspect directory: " + path_to_utf8(entry.path()));
            continue; // recurse (default behavior; symlinks never followed)
        }

        if (!entry.is_regular_file(ec2))
            fail("unsupported filesystem entry (neither regular file nor directory): " +
                 path_to_utf8(entry.path()));

        FileRecord rec;
        rec.absolute = entry.path();
        uint64_t size = uint64_t(entry.file_size(ec2));
        if (ec2) fail("cannot get file size: " + path_to_utf8(entry.path()));
        rec.size = size;
        std::error_code rel_ec;
        auto rel = entry.path().lexically_relative(root);
        if (rel.empty() || rel_ec) fail("cannot relativize path: " + path_to_utf8(entry.path()));
        // Normalize to '/' separators for the manifest.
        std::wstring relw = rel.wstring();
        for (wchar_t& c : relw)
            if (c == L'\\') c = L'/';
        rec.rel_utf8 = path_to_utf8(std::filesystem::path(relw));
        payload::validate_payload_path(rec.rel_utf8); // defense in depth on our own output
        out.push_back(std::move(rec));
    }

    std::sort(out.begin(), out.end(),
              [](const FileRecord& a, const FileRecord& b) { return a.rel_utf8 < b.rel_utf8; });
    return out;
}

void check_conflicts(const std::filesystem::path& out_dir,
                     const std::vector<payload::Entry>& entries) {
    std::vector<std::string> conflicts;
    for (const auto& e : entries) {
        std::filesystem::path target = out_dir / utf8_to_path(e.path);
        std::error_code ec;
        if (std::filesystem::exists(target, ec)) conflicts.push_back(e.path);
    }
    if (!conflicts.empty()) {
        std::string msg = "refusing to overwrite existing files (" +
                          std::to_string(conflicts.size()) + "): " + conflicts.front();
        if (conflicts.size() > 1) msg += " (and " + std::to_string(conflicts.size() - 1) + " more)";
        fail(msg);
    }
}

StagedExtraction::~StagedExtraction() {
    if (!committed && !staging_dir.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(staging_dir, ec); // best effort
    }
}

StagedExtraction::StagedExtraction(StagedExtraction&& other) noexcept
    : staging_dir(std::move(other.staging_dir)),
      staged(std::move(other.staged)),
      file_count(other.file_count),
      committed(other.committed) {
    other.staging_dir.clear();
    other.committed = true; // prevent double cleanup
}

StagedExtraction& StagedExtraction::operator=(StagedExtraction&& other) noexcept {
    if (this != &other) {
        staging_dir = std::move(other.staging_dir);
        staged = std::move(other.staged);
        file_count = other.file_count;
        committed = other.committed;
        other.staging_dir.clear();
        other.committed = true;
    }
    return *this;
}

StagedExtraction stage_payload(const std::filesystem::path& out_dir,
                               const std::vector<payload::Entry>& entries,
                               const DataReader& reader, uint32_t& running_crc) {
    // Staging directory name: pid + tick to avoid collisions.
    wchar_t name[96];
    swprintf(name, 96, L".gputxrx-stage-%lu-%llu", GetCurrentProcessId(),
             (unsigned long long)GetTickCount64());
    StagedExtraction st;
    st.staging_dir = out_dir / name;
    std::error_code ec;
    if (!std::filesystem::create_directories(st.staging_dir, ec))
        fail("cannot create staging directory " + path_to_utf8(st.staging_dir) + ": " +
             ec.message());

    const uint32_t kChunk = 1u << 20; // 1 MiB streaming chunks
    std::vector<uint8_t> buf(kChunk);
    uint64_t consumed = 0;

    for (const auto& e : entries) {
        std::filesystem::path staged_path = st.staging_dir / utf8_to_path(e.path);
        std::filesystem::path parent = staged_path.parent_path();
        if (!parent.empty()) {
            std::error_code ec2;
            if (!std::filesystem::create_directories(parent, ec2) &&
                !std::filesystem::is_directory(parent, ec2))
                fail("cannot create output subdirectory for " + e.path + ": " + ec2.message());
        }

        FILE* f = nullptr;
        if (_wfopen_s(&f, staged_path.c_str(), L"wb") != 0 || !f)
            fail("cannot open staged file for writing: " + e.path);

        Md5 md5;
        uint64_t written = 0;
        bool io_error = false;
        std::string io_msg;
        while (written < e.size) {
            uint32_t take = uint32_t(std::min<uint64_t>(kChunk, e.size - written));
            const uint8_t* p = reader(e.data_offset + written, take);
            if (!p) {
                io_msg = "payload read failed at offset " + std::to_string(e.data_offset + written);
                io_error = true;
                break;
            }
            md5.update(p, take);
            running_crc = crc32(p, take, running_crc);
            if (fwrite(p, 1, take, f) != take) {
                io_msg = "disk write failed (staged file for " + e.path + ")";
                io_error = true;
                break;
            }
            written += take;
            consumed += take;
        }
        if (!io_error && e.size == 0) {
            // MD5 of the empty stream still needs finalization below.
        }
        if (!io_error) {
            uint8_t digest[Md5::kDigestSize];
            md5.final(digest);
            if (std::memcmp(digest, e.md5.data(), 16) != 0) {
                io_msg = "MD5 mismatch for " + e.path + " (payload corrupted?)";
                io_error = true;
            }
        }
        if (fclose(f) != 0 && !io_error) {
            io_msg = "close failed on staged file for " + e.path;
            io_error = true;
        }
        if (io_error) fail(io_msg); // destructor removes the staging dir

        st.staged.emplace_back(staged_path, out_dir / utf8_to_path(e.path));
        st.file_count++;
    }

    uint64_t expected_total = 0;
    for (const auto& e : entries) expected_total += e.size;
    if (consumed != expected_total)
        fail("internal error: consumed " + std::to_string(consumed) + " of " +
             std::to_string(expected_total) + " payload bytes");
    return st;
}

void commit_staged(StagedExtraction& st) {
    std::vector<std::filesystem::path> moved;
    for (auto& [staged_path, final_path] : st.staged) {
        std::error_code ec;
        std::filesystem::path parent = final_path.parent_path();
        if (!parent.empty() && !std::filesystem::is_directory(parent, ec)) {
            std::error_code mk;
            if (!std::filesystem::create_directories(parent, mk)) {
                // Roll back what we moved so far.
                for (const auto& m : moved) std::filesystem::remove(m, ec);
                fail("commit failed: cannot create directory " + path_to_utf8(parent) + ": " +
                     mk.message());
            }
        }
        std::filesystem::rename(staged_path, final_path, ec);
        if (ec) {
            for (const auto& m : moved) std::filesystem::remove(m, ec);
            fail("commit failed: cannot move " + path_to_utf8(staged_path) + " -> " +
                 path_to_utf8(final_path) + ": " + ec.message() +
                 " (target may have appeared concurrently; some files may have been rolled back)");
        }
        moved.push_back(final_path);
    }
    std::error_code ec;
    std::filesystem::remove_all(st.staging_dir, ec);
    st.committed = true;
}

} // namespace gputxrx::storage
