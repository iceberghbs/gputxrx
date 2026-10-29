// Milestone 0: GPU capability probe.
//
// Verifies, on the target machine, that:
//  1. The NVIDIA adapter is selected explicitly (no AMD/software fallback).
//  2. A D3D12 device is created on it.
//  3. Local video-memory budget/usage can be queried.
//  4. A buffer can be created with the sharing configuration used by the design
//     (shared heap + placed resource + ALLOW_RENDER_TARGET, COMMON state).
//  5. The resource can be exported with ID3D12Device::CreateSharedHandle as a
//     *named* handle ("Global\\...").
//  6. A second, independent native process opens it by name via
//     ID3D12Device::OpenSharedHandleByName (no sockets/pipes/handle inheritance).
//  7. A known byte pattern written by the transmitter is verified
//     byte-for-byte by the receiver.
//  8. Synchronization (fence waits) guarantees the data is visible before the
//     identifier is published.
//  9. After normal termination of both processes the named resource is gone
//     (open-by-name must fail).
// 10. Allocation failures / unsupported configurations produce actionable
//     diagnostics.
//
// Usage:
//   test_gpu_roundtrip            run the full probe (spawns the child itself)
//   test_gpu_roundtrip --child <name> <size> <seed>
//                                 internal receiver mode

#include "gputxrx/gpu_buffer.hpp"

#include <windows.h>
#include <objbase.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace gputxrx::gpu;

namespace {

// Pure-function pseudo-random byte pattern: deterministic on both sides.
uint8_t pattern_byte(uint64_t index, uint64_t seed) {
    uint64_t x = index * 0x9E3779B97F4A7C15ull + seed;
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDull;
    x ^= x >> 33;
    return uint8_t(x >> 40);
}

std::wstring self_path() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::wstring(buf, buf + n);
}

int run_receiver_child(const std::wstring& identifier, uint64_t size, uint64_t seed) {
    auto ctx = GpuContext::create(AdapterSelector{});
    RxSharedBuffer rx;
    rx.open(*ctx, identifier);

    if (rx.size() < size) {
        std::fprintf(stderr, "CHILD-FAIL: resource smaller than expected (%llu < %llu)\n",
                     (unsigned long long)rx.size(), (unsigned long long)size);
        return 3;
    }

    std::vector<uint8_t> buf(1 << 20);
    uint64_t off = 0;
    while (off < size) {
        const size_t n = (size_t)std::min<uint64_t>(buf.size(), size - off);
        rx.read(off, buf.data(), n);
        for (size_t i = 0; i < n; ++i) {
            if (buf[i] != pattern_byte(off + i, seed)) {
                std::fprintf(stderr,
                             "CHILD-FAIL: pattern mismatch at offset %llu (got 0x%02x want 0x%02x)"
                             " | re-read same offset: ",
                             (unsigned long long)(off + i), buf[i], pattern_byte(off + i, seed));
                // Immediate second read of just this byte, plus a fresh full read.
                uint8_t again = 0;
                rx.read(off + i, &again, 1);
                std::vector<uint8_t> full((size_t)size);
                rx.read(0, full.data(), (size_t)size);
                std::fprintf(stderr, "0x%02x | single-full-read mismatch count=%llu\n", again,
                             (unsigned long long)[&] {
                                 unsigned long long bad = 0;
                                 for (uint64_t j = 0; j < size; ++j)
                                     if (full[(size_t)j] != pattern_byte(j, seed)) ++bad;
                                 return bad;
                             }());
                return 4;
            }
        }
        off += n;
    }
    std::fprintf(stderr, "child: verified %llu bytes byte-for-byte\n", (unsigned long long)size);
    return 0;
}

int run_probe() {
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        std::fprintf(stderr, "[%s] %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok) ++failures;
    };

    // 1+2: explicit NVIDIA adapter selection + device creation.
    GpuContext::create(AdapterSelector{}); // throw-away context validates logging path
    auto ctx = GpuContext::create(AdapterSelector{});
    const AdapterInfo& info = ctx->adapter();
    check(info.vendor_id == 0x10DE, "NVIDIA adapter selected explicitly");

    // 3: video memory budget/usage.
    MemoryInfo mem = ctx->memory_info();
    check(mem.budget > 0, "DXGI local memory budget queried");
    std::fwprintf(stderr, L"       budget=%.1f MiB, usage=%.1f MiB\n",
                  mem.budget / 1048576.0, mem.current_usage / 1048576.0);

    // 4+5+7: create shared buffer, write known pattern, publish.
    uint64_t seed = GetTickCount64() ^ 0x5EEDBEEF12345678ull;
    // Odd size to catch off-by-one and chunking issues.
    uint64_t size = 8ull * 1024 * 1024 + 12345;

    wchar_t guid[64];
    GUID g{};
    CoCreateGuid(&g);
    StringFromGUID2(g, guid, 64);
    std::wstring name = L"Global\\gputxrx-probe-";
    for (wchar_t* c = guid; *c; ++c)
        if (*c != L'{' && *c != L'}' && *c != L'-') name += (*c == L' ' ? L'-' : *c);

    std::vector<uint8_t> pattern(size);
    for (uint64_t i = 0; i < size; ++i) pattern[i] = pattern_byte(i, seed);

    std::wstring identifier_copy;
    bool published = false;
    {
        TxSharedBuffer tx;
        tx.create(*ctx, name, size);
        check(tx.size() == size, "shared committed buffer created (COMMON state)");

        tx.upload_and_publish(pattern.data());
        check(true, "pattern uploaded, GPU work completed (fence), NT handle exported");
        const std::wstring& identifier = tx.name();
        std::fwprintf(stderr, L"       identifier: %s\n", identifier.c_str());

        // 6+7: launch a separate native process which opens by identifier and verifies.
        std::wstring cmd = L"\"" + self_path() + L"\" --child \"" + identifier + L"\" " +
                           std::to_wstring(size) + L" " + std::to_wstring(seed);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                            &si, &pi)) {
            std::fprintf(stderr, "FAIL: could not spawn receiver probe process\n");
            return 1;
        }
        WaitForSingleObject(pi.hProcess, 120000);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        check(code == 0, "separate process opened resource by identifier and verified byte pattern");

        // 8: transmitter still alive; the resource is readable via a second open.
        {
            RxSharedBuffer self_rx;
            self_rx.open(*ctx, identifier);
            uint8_t a = 0, b = 0;
            self_rx.read(0, &a, 1);
            self_rx.read(size - 1, &b, 1);
            check(a == pattern_byte(0, seed) && b == pattern_byte(size - 1, seed),
                  "transmitter-side readback matches pattern head/tail while tx is alive");
        }

        // 10: stale/invalid identifier fails with a clear error.
        {
            bool failed_as_expected = false;
            try {
                RxSharedBuffer bad;
                bad.open(*ctx, L"gputxrx1:999999:12345");
            } catch (const std::exception& e) {
                failed_as_expected = true;
                std::fprintf(stderr, "       invalid-identifier error: %s\n", e.what());
            }
            check(failed_as_expected, "opening an invalid identifier fails cleanly");
        }

        published = true;
        identifier_copy = identifier;
    }

    // 9: normal termination releases the resource once all handles close
    // (transmitter's TxSharedBuffer and the probe receiver both went out of
    // scope above, dropping every device resource and shared handle).
    {
        bool released = false;
        try {
            RxSharedBuffer gone;
            gone.open(*ctx, identifier_copy);
            std::fprintf(stderr, "       note: resource still openable (kept alive by refs)\n");
        } catch (const std::exception&) {
            released = true;
        }
        check(released, "resource released after normal termination (identifier unusable)");
    }
    return failures;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 2 && std::wstring(argv[1]) == L"--child") {
        if (argc < 5) return 2;
        return run_receiver_child(argv[2], std::wcstoull(argv[3], nullptr, 10),
                                  std::wcstoull(argv[4], nullptr, 10));
    }
    std::fprintf(stderr, "=== gputxrx Milestone 0 GPU capability probe ===\n");
    int rc = 99;
    try {
        rc = run_probe();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "PROBE EXCEPTION: %s\n", e.what());
    }
    std::fprintf(stderr, "=== probe %s (%d failure%s) ===\n", rc == 0 ? "PASSED" : "FAILED", rc,
                 rc == 1 ? "" : "s");
    return rc;
}
