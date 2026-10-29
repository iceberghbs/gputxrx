#include "gputxrx/cli.hpp"

#include <windows.h>
#include <objbase.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "gputxrx/checksum.hpp"
#include "gputxrx/gpu_buffer.hpp"
#include "gputxrx/payload.hpp"
#include "gputxrx/storage.hpp"

namespace gputxrx::cli {

namespace {

using hrc = std::chrono::steady_clock;

double ms_since(hrc::time_point t0) {
    return std::chrono::duration<double, std::milli>(hrc::now() - t0).count();
}

struct Limits {
    uint64_t max_payload_bytes = 2048ull << 20;   // application-level cap
    uint64_t safety_margin_bytes = 256ull << 20;  // reserved VRAM headroom
};

[[noreturn]] void usage_error(const std::string& msg) {
    std::fprintf(stderr, "gputxrx: error: %s\n", msg.c_str());
    std::fprintf(stderr,
                 "usage:\n"
                 "  gputxrx tx <payload_dir|payload_file> [--max-payload-mb N] [--margin-mb N]\n"
                 "  gputxrx rx <identifier> [output_dir]\n");
    std::exit(2);
}

uint64_t env_or_flag_mb(const wchar_t* env_name, std::optional<uint64_t> flag_mb,
                        uint64_t default_mb, const char* what) {
    if (flag_mb) return *flag_mb * (1ull << 20);
    wchar_t buf[64] = {0};
    if (GetEnvironmentVariableW(env_name, buf, 64) > 0) {
        unsigned long long v = 0;
        if (swscanf_s(buf, L"%llu", &v) == 1 && v > 0) return v * (1ull << 20);
    }
    (void)what;
    return default_mb * (1ull << 20);
}

// ---------------------------------------------------------------------------
// tx
// ---------------------------------------------------------------------------

int run_tx(const std::wstring& dir_arg, const Limits& limits) {
    std::filesystem::path root(dir_arg);
    Limits lim = limits;

    auto t_start = hrc::now();
    std::printf("gputxrx tx: enumerating %ls\n", dir_arg.c_str());

    // The input may be a directory or a single file of any type. Payload
    // paths are always rooted at the input's own name, so the receiver
    // recreates the top-level folder (or the single file) verbatim.
    std::error_code fs_ec;
    if (!std::filesystem::exists(root, fs_ec)) {
        std::fprintf(stderr, "gputxrx: error: input path does not exist: %ls\n",
                     dir_arg.c_str());
        return 1;
    }

    std::vector<storage::FileRecord> files;
    if (std::filesystem::is_regular_file(root, fs_ec)) {
        auto sym = std::filesystem::symlink_status(root, fs_ec);
        if (fs_ec || std::filesystem::is_symlink(sym)) {
            std::fprintf(stderr, "gputxrx: error: input file is a symlink, which is not "
                                 "supported: %ls\n",
                         dir_arg.c_str());
            return 1;
        }
        std::filesystem::path abs = std::filesystem::weakly_canonical(root, fs_ec);
        if (fs_ec) abs = std::filesystem::absolute(root);
        storage::FileRecord rec;
        rec.absolute = abs;
        uint64_t size = uint64_t(std::filesystem::file_size(abs, fs_ec));
        if (fs_ec) {
            std::fprintf(stderr, "gputxrx: error: cannot get file size: %ls\n",
                         dir_arg.c_str());
            return 1;
        }
        rec.size = size;
        rec.rel_utf8 = storage::path_to_utf8(std::filesystem::path(abs.filename()));
        payload::validate_payload_path(rec.rel_utf8);
        files.push_back(std::move(rec));
    } else {
        files = storage::collect_files(root);
        // Preserve the top-level folder in the payload: prefix every relative
        // path with the input directory's own name.
        std::filesystem::path abs = std::filesystem::weakly_canonical(root, fs_ec);
        if (fs_ec) abs = std::filesystem::absolute(root);
        std::filesystem::path namep = abs.filename();
        if (namep.empty()) namep = abs.parent_path().filename();
        if (namep.empty() || namep.wstring() == L"." || namep.wstring() == L"..") {
            std::fprintf(stderr,
                         "gputxrx: error: cannot determine a top-level folder name for "
                         "'%ls'; pass a named directory or file\n",
                         dir_arg.c_str());
            return 1;
        }
        std::string root_prefix = storage::path_to_utf8(namep) + "/";
        for (auto& f : files) {
            f.rel_utf8 = root_prefix + f.rel_utf8;
            payload::validate_payload_path(f.rel_utf8);
        }
    }

    const double enum_ms = ms_since(t_start);
    std::printf("  files: %zu\n", files.size());

    // Layout (checked arithmetic).
    std::vector<payload::LayoutInput> inputs;
    inputs.reserve(files.size());
    for (const auto& f : files) inputs.push_back({f.rel_utf8, f.size});
    auto layout = payload::plan_layout(inputs);
    std::printf("  payload: %llu bytes (manifest %llu, data %llu)\n",
                (unsigned long long)layout.total_size,
                (unsigned long long)layout.manifest_size,
                (unsigned long long)layout.data_region_size);

    // GPU context + memory-budget precheck, before any GPU allocation.
    auto ctx = gpu::GpuContext::create(gpu::AdapterSelector{});
    gpu::MemoryInfo mem = ctx->memory_info();
    std::printf("  GPU: %ls budget=%.1f MiB usage=%.1f MiB\n",
                ctx->adapter().description.c_str(), mem.budget / 1048576.0,
                mem.current_usage / 1048576.0);

    uint64_t usable = 0;
    if (mem.budget > mem.current_usage + lim.safety_margin_bytes)
        usable = mem.budget - mem.current_usage - lim.safety_margin_bytes;
    uint64_t safe_limit = std::min(usable, lim.max_payload_bytes);
    std::printf("  limits: app max %.1f MiB, margin %.1f MiB, safe limit %.1f MiB\n",
                lim.max_payload_bytes / 1048576.0, lim.safety_margin_bytes / 1048576.0,
                safe_limit / 1048576.0);
    if (layout.total_size > safe_limit) {
        std::fprintf(stderr,
                     "gputxrx: error: serialized payload (%.1f MiB) exceeds the safe limit "
                     "(%.1f MiB = min(adapter budget %.1f - usage %.1f - margin %.1f, app max)). "
                     "No fallback transport is attempted.\n",
                     layout.total_size / 1048576.0, safe_limit / 1048576.0,
                     mem.budget / 1048576.0, mem.current_usage / 1048576.0,
                     lim.safety_margin_bytes / 1048576.0);
        return 1;
    }

    // Serialize: header region is written last; file data streams in with MD5.
    auto t_ser = hrc::now();
    std::vector<uint8_t> buf;
    try {
        buf.assign(size_t(layout.total_size), 0);
    } catch (const std::bad_alloc&) {
        std::fprintf(stderr,
                     "gputxrx: error: host memory allocation failed for a %llu-byte staging "
                     "buffer (payload is staged in RAM before upload)\n",
                     (unsigned long long)layout.total_size);
        return 1;
    }

    std::vector<payload::Entry> entries(files.size());
    const uint32_t kReadChunk = 1u << 20;
    std::vector<uint8_t> chunk(kReadChunk);
    auto t_hash = hrc::now();
    double read_ms = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        auto& e = entries[i];
        e.path = files[i].rel_utf8;
        e.size = files[i].size;
        e.data_offset = layout.data_offsets[i];

        FILE* f = nullptr;
        if (_wfopen_s(&f, files[i].absolute.c_str(), L"rb") != 0 || !f) {
            std::fprintf(stderr, "gputxrx: error: cannot open input file: %ls\n",
                         files[i].absolute.c_str());
            return 1;
        }
        Md5 md5;
        uint64_t got = 0;
        auto t_read = hrc::now();
        while (got < e.size) {
            uint32_t take = uint32_t(std::min<uint64_t>(kReadChunk, e.size - got));
            size_t n = fread(chunk.data(), 1, take, f);
            if (n != take) {
                std::fprintf(stderr,
                             "gputxrx: error: file changed while being read (expected %llu bytes, "
                             "got %zu at %llu): %ls\n",
                             (unsigned long long)e.size, n, (unsigned long long)got,
                             files[i].absolute.c_str());
                fclose(f);
                return 1;
            }
            md5.update(chunk.data(), take);
            std::memcpy(buf.data() + e.data_offset + got, chunk.data(), take);
            got += take;
        }
        read_ms += ms_since(t_read);
        fclose(f);
        md5.final(e.md5.data());
    }
    const double hash_ms = ms_since(t_hash);

    // Transfer id: 16 random bytes.
    payload::Header hdr;
    GUID g{};
    CoCreateGuid(&g);
    std::memcpy(hdr.transfer_id.data(), &g, 16);
    hdr.total_size = layout.total_size;
    hdr.manifest_offset = layout.manifest_offset;
    hdr.manifest_size = layout.manifest_size;
    hdr.file_count = files.size();
    hdr.data_region_offset = layout.data_region_offset;
    hdr.data_region_size = layout.data_region_size;

    // Entries table + path blob.
    uint64_t path_off = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        payload::write_manifest_entry(buf.data(), layout.manifest_offset, uint32_t(i), path_off,
                                      entries[i]);
        std::memcpy(buf.data() + layout.path_blob_offset + path_off, entries[i].path.data(),
                    entries[i].path.size());
        path_off += entries[i].path.size();
    }
    // Write the header with crc = 0 first, then compute the CRC-32 over the
    // whole final buffer (so the receiver, which zeroes only the crc field,
    // computes the same value) and patch it in.
    payload::write_header(buf.data(), hdr); // hdr.payload_crc32 == 0 here
    hdr.payload_crc32 = crc32(buf.data(), buf.size());
    payload::put_u32(buf.data() + payload::kCrcFieldOffset, hdr.payload_crc32);
    const double ser_ms = ms_since(t_ser);

    // GPU buffer + upload + publish.
    auto t_gpu = hrc::now();
    gpu::TxSharedBuffer tx;
    tx.create(*ctx, L"gputxrx", layout.total_size);
    tx.upload_and_publish(buf.data());
    const double gpu_ms = ms_since(t_gpu);
    buf.clear();
    buf.shrink_to_fit();

    const std::wstring& id = tx.name();
    std::printf("\n");
    std::printf("================ COPY THIS IDENTIFIER ================\n");
    std::printf("%ls\n", id.c_str());
    std::printf("======================================================\n");
    std::printf("\n");
    std::printf("Receiver command (run in another terminal):\n");
    std::printf("  gputxrx rx '%ls' [output_dir]\n", id.c_str());
    std::printf("\n");
    std::printf("Timings: enumerate %.0f ms, hash+serialize %.0f ms (file read %.0f ms),\n"
                "         GPU upload %.0f ms (%.1f MiB/s)\n",
                enum_ms, ser_ms, read_ms, gpu_ms,
                gpu_ms > 0 ? layout.total_size / 1048576.0 / (gpu_ms / 1000.0) : 0.0);
    std::printf("Transmitter stays alive and keeps the GPU resource published.\n");
    std::printf("Press Ctrl+C here (or type 'q' + Enter) to terminate the transfer.\n");
    std::fflush(stdout);

    // Keep alive indefinitely. The resource is released when this process
    // exits or crashes; a receiver that already opened it may keep the
    // resource alive beyond that (documented resource semantics).
    SetConsoleCtrlHandler(nullptr, FALSE); // default Ctrl+C handling terminates us
    std::wstring line;
    for (;;) {
        if (std::wcin.eof()) {
            Sleep(1000); // stdin closed (e.g. background run): keep publishing
            continue;
        }
        if (!std::getline(std::wcin, line)) {
            std::wcin.clear();
            Sleep(200);
            continue;
        }
        if (line == L"q" || line == L"quit" || line == L"exit") break;
    }
    std::printf("transmitter: releasing resources and exiting.\n");
    return 0;
}

// ---------------------------------------------------------------------------
// rx
// ---------------------------------------------------------------------------

int run_rx(const std::wstring& id_arg, const std::wstring& out_arg) {
    std::filesystem::path out_dir =
        out_arg.empty() ? std::filesystem::current_path() : std::filesystem::path(out_arg);

    auto t_start = hrc::now();
    auto ctx = gpu::GpuContext::create(gpu::AdapterSelector{});
    gpu::RxSharedBuffer rx;
    rx.open(*ctx, id_arg);
    const uint64_t resource_size = rx.size();
    std::printf("gputxrx rx: opened shared GPU resource (%.1f MiB), output: %ls\n",
                resource_size / 1048576.0, out_dir.c_str());

    // Readback buffer <= kMaxReadbackChunk bounds GPU memory usage.
    const uint32_t kChunk = uint32_t(std::min<uint64_t>(gpu::kMaxReadbackChunk,
                                                        resource_size));
    std::vector<uint8_t> chunk(size_t(kChunk), 0);

    auto read = [&](uint64_t off, uint32_t len) -> const uint8_t* {
        if (len == 0) return chunk.data();
        if (off > resource_size || len > resource_size - off)
            throw std::runtime_error("readback out of resource bounds");
        if (len > kChunk) throw std::runtime_error("internal error: read chunk too large");
        rx.read(off, chunk.data(), len);
        return chunk.data();
    };

    // Pass 1: header + manifest. CRC starts with the header, crc field zeroed.
    auto t_read1 = hrc::now();
    if (resource_size < payload::kHeaderSize)
        throw std::runtime_error("GPU resource too small to hold a payload header");
    {
        uint8_t header_copy[payload::kHeaderSize];
        std::memcpy(header_copy, read(0, payload::kHeaderSize), payload::kHeaderSize);
        payload::Header hdr_probe = payload::parse_header(header_copy, payload::kHeaderSize);
        if (hdr_probe.total_size > resource_size)
            throw std::runtime_error("payload header claims " +
                                     std::to_string(hdr_probe.total_size) +
                                     " bytes but the GPU resource holds " +
                                     std::to_string(resource_size));
    }

    // Read [0, manifest end) into one buffer so manifest parsing sees the full
    // region (paths live in the blob right after the entries table).
    uint8_t header_copy[payload::kHeaderSize];
    std::memcpy(header_copy, read(0, payload::kHeaderSize), payload::kHeaderSize);
    payload::Header hdr = payload::parse_header(header_copy, payload::kHeaderSize);

    uint32_t crc = crc32(header_copy, payload::kCrcFieldOffset);
    const uint8_t zeros[4] = {0, 0, 0, 0};
    crc = crc32(zeros, 4, crc);
    crc = crc32(header_copy + payload::kCrcFieldOffset + 4,
                payload::kHeaderSize - payload::kCrcFieldOffset - 4, crc);

    const uint64_t hm_size = hdr.manifest_offset + hdr.manifest_size;
    std::vector<uint8_t> hm(size_t(hm_size), 0);
    uint64_t done = 0;
    while (done < hm_size) {
        uint32_t take = uint32_t(std::min<uint64_t>(kChunk, hm_size - done));
        const uint8_t* p = read(done, take);
        std::memcpy(hm.data() + done, p, take);
        // The header bytes were already folded into the CRC above (with the
        // crc field zeroed); only count bytes beyond the header.
        uint64_t cstart = std::max<uint64_t>(done, payload::kHeaderSize);
        uint64_t cend = done + take;
        if (cend > cstart) crc = crc32(p + (cstart - done), uint32_t(cend - cstart), crc);
        done = cend;
    }
    auto entries = payload::parse_manifest(hm.data(), hdr);
    const double read1_ms = ms_since(t_read1);
    std::printf("  manifest: %llu files validated\n", (unsigned long long)entries.size());

    // Output directory.
    std::error_code ec;
    if (!std::filesystem::exists(out_dir)) {
        if (!std::filesystem::create_directories(out_dir, ec))
            throw std::runtime_error("cannot create output directory '" +
                                     storage::path_to_utf8(out_dir) + "': " + ec.message());
    } else if (!std::filesystem::is_directory(out_dir, ec)) {
        throw std::runtime_error("output path exists and is not a directory: " +
                                 storage::path_to_utf8(out_dir));
    }

    // Conflicts before any staging or commit.
    storage::check_conflicts(out_dir, entries);

    // Pass 2: stream file data into staging, verifying MD5 per file.
    auto t_read2 = hrc::now();
    storage::StagedExtraction staged = storage::stage_payload(
        out_dir, entries,
        [&](uint64_t off, uint32_t len) -> const uint8_t* { return read(off, len); }, crc);
    const double read2_ms = ms_since(t_read2);

    // Whole-payload integrity.
    if (crc != hdr.payload_crc32)
        throw std::runtime_error("whole-payload CRC-32 mismatch (read " +
                                 std::to_string(crc) + ", header says " +
                                 std::to_string(hdr.payload_crc32) +
                                 "); the transfer is incomplete or corrupted");

    // Commit.
    auto t_commit = hrc::now();
    storage::commit_staged(staged);
    const double commit_ms = ms_since(t_commit);
    const double total_ms = ms_since(t_start);

    std::printf("  readback+verify: header/manifest %.0f ms, data %.0f ms (%.1f MiB/s),\n"
                "           commit %.0f ms, total %.0f ms\n",
                read1_ms, read2_ms,
                read2_ms > 0 ? hdr.data_region_size / 1048576.0 / (read2_ms / 1000.0) : 0.0,
                commit_ms, total_ms);
    std::printf("gputxrx rx: extracted %llu files to %ls (verified: per-file MD5 + payload "
                "CRC-32)\n",
                (unsigned long long)staged.file_count, out_dir.c_str());
    return 0;
}

std::optional<uint64_t> parse_mb_flag(const std::wstring& v) {
    wchar_t* end = nullptr;
    unsigned long long n = std::wcstoull(v.c_str(), &end, 10);
    if (end != v.c_str() + v.size() || n == 0) return std::nullopt;
    return n;
}

} // namespace

int run(int argc, wchar_t** argv) {
    if (argc < 2) usage_error("missing command");
    std::wstring cmd = argv[1];

    if (cmd == L"tx") {
        if (argc < 3) usage_error("tx requires a payload directory or file");
        Limits lim;
        lim.max_payload_bytes =
            env_or_flag_mb(L"GPUTXRX_MAX_PAYLOAD_MB", std::nullopt, 2048, "max payload");
        lim.safety_margin_bytes =
            env_or_flag_mb(L"GPUTXRX_SAFETY_MARGIN_MB", std::nullopt, 256, "margin");
        for (int i = 3; i < argc; ++i) {
            std::wstring a = argv[i];
            if (a == L"--max-payload-mb" && i + 1 < argc) {
                auto v = parse_mb_flag(argv[++i]);
                if (!v) usage_error("--max-payload-mb requires a positive integer");
                lim.max_payload_bytes = *v * (1ull << 20);
            } else if (a == L"--margin-mb" && i + 1 < argc) {
                auto v = parse_mb_flag(argv[++i]);
                if (!v) usage_error("--margin-mb requires a positive integer");
                lim.safety_margin_bytes = *v * (1ull << 20);
            } else {
                usage_error("unknown tx argument: " + std::string(a.begin(), a.end()));
            }
        }
        return run_tx(argv[2], lim);
    }

    if (cmd == L"rx") {
        if (argc < 3) usage_error("rx requires the identifier printed by the transmitter");
        std::wstring out = argc >= 4 ? argv[3] : L"";
        return run_rx(argv[2], out);
    }

    if (cmd == L"--help" || cmd == L"-h" || cmd == L"help") {
        std::printf("gputxrx - transfer a directory between local processes via a shared GPU "
                    "buffer (D3D12, NVIDIA)\n\n"
                    "usage:\n"
                    "  gputxrx tx <payload_dir|payload_file> [--max-payload-mb N] [--margin-mb N]\n"
                    "  gputxrx rx <identifier> [output_dir]\n\n"
                    "The transmitter prints an identifier; copy it into the receiver command.\n"
                    "Payload paths are rooted at the input's own name: passing .\\hello\\ keeps\n"
                    "hello/ at the top of the extracted tree; passing a single file keeps that\n"
                    "file name. Any file type is accepted.\n"
                    "Env overrides: GPUTXRX_MAX_PAYLOAD_MB (default 2048), "
                    "GPUTXRX_SAFETY_MARGIN_MB (default 256).\n");
        return 0;
    }

    usage_error("unknown command: " + std::string(cmd.begin(), cmd.end()));
    return 2;
}

} // namespace gputxrx::cli
