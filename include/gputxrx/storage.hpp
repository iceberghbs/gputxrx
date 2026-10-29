#pragma once
// Filesystem enumeration, staging extraction, and commit.
//
// Enumeration includes hidden files/directories and dot-directories such as
// .git. Symbolic links and other reparse points are rejected with a clear
// error, never skipped or followed.
//
// Extraction is staged: all files are written under
//   <output_dir>/.gputxrx-stage-<pid>-<random>/
// and only moved to their final locations after per-file MD5 verification and
// the caller's payload-level checks succeed. The staging area is removed on
// any failure path. Commit is not atomic across the whole tree (a sequence of
// renames); on commit failure, already-moved files are removed on a
// best-effort basis and the error lists affected paths.

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "gputxrx/payload.hpp"

namespace gputxrx::storage {

class StorageError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct FileRecord {
    std::filesystem::path absolute; // wide path on Windows
    std::string rel_utf8;           // relative path, '/' separators, UTF-8
    uint64_t size = 0;
};

// Recursively collects regular files under `root` (hidden included), sorted by
// relative path. Throws StorageError on symlinks/reparse points, unreadable
// entries, or if the root is not a real directory.
std::vector<FileRecord> collect_files(const std::filesystem::path& root);

std::string path_to_utf8(const std::filesystem::path& p);
std::filesystem::path utf8_to_path(const std::string& s);

// Streams payload data into a staging area, verifying per-file MD5 while
// writing. `reader(offset, len)` returns a pointer valid until the next call;
// len never exceeds gpu::kMaxReadbackChunk. `running_crc` is updated with the
// CRC-32 of every data byte consumed (for the whole-payload check). Throws on
// the first failure and removes everything staged so far.
struct StagedExtraction {
    std::filesystem::path staging_dir;
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> staged; // staged -> final
    size_t file_count = 0;
    bool committed = false;

    StagedExtraction() = default;
    ~StagedExtraction(); // removes the staging dir unless committed
    StagedExtraction(StagedExtraction&&) noexcept;
    StagedExtraction& operator=(StagedExtraction&&) noexcept;
    StagedExtraction(const StagedExtraction&) = delete;
    StagedExtraction& operator=(const StagedExtraction&) = delete;
};

using DataReader = std::function<const uint8_t*(uint64_t offset, uint32_t len)>;

// Pre-checks: no destination path may already exist. Throws StorageError
// listing all conflicts. `out_dir` must already exist.
void check_conflicts(const std::filesystem::path& out_dir,
                     const std::vector<payload::Entry>& entries);

StagedExtraction stage_payload(const std::filesystem::path& out_dir,
                               const std::vector<payload::Entry>& entries,
                               const DataReader& reader, uint32_t& running_crc);

// Moves staged files to their final locations (creating parent directories).
// Best-effort rollback of already-moved files on failure. Sets `committed`.
void commit_staged(StagedExtraction& st);

} // namespace gputxrx::storage
