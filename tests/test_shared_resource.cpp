// Shared-resource tests that can run in a single process: create -> open by
// identifier in the same process, plus malformed/stale identifier rejection.
// (Cross-process open is covered by test_gpu_roundtrip and the lifetime test.)
#include "gputxrx/gpu_buffer.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace gputxrx::gpu;

static int failures = 0;

static void expect(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

static uint8_t pattern_byte(uint64_t i, uint64_t seed) {
    uint64_t x = i * 0x9E3779B97F4A7C15ull + seed;
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDull;
    x ^= x >> 33;
    return uint8_t(x >> 40);
}

int wmain() {
    const uint64_t size = 1 << 20; // 1 MiB
    const uint64_t seed = 0xABCDEF01;
    auto ctx = GpuContext::create(AdapterSelector{});

    TxSharedBuffer tx;
    tx.create(*ctx, L"gputxrx", size);
    std::vector<uint8_t> data(size);
    for (uint64_t i = 0; i < size; ++i) data[i] = pattern_byte(i, seed);
    tx.upload_and_publish(data.data());

    bool opened = false;
    try {
        RxSharedBuffer rx;
        rx.open(*ctx, tx.name());
        opened = true;
        std::vector<uint8_t> back(size);
        rx.read(0, back.data(), (size_t)size);
        expect(std::memcmp(back.data(), data.data(), size) == 0,
               "same-process open by identifier returns identical bytes");
    } catch (const std::exception& e) {
        std::printf("       open failed: %s\n", e.what());
    }
    expect(opened, "same-process open by identifier succeeds");

    bool malformed_rejected = true;
    const wchar_t* bad_ids[] = {L"", L"gputxrx", L"gputxrx:", L"gputxrx:abc:1",
                                L"gputxrx:1:xyz", L"Global\\gputxrx-nope"};
    for (const wchar_t* bad : bad_ids) {
        try {
            RxSharedBuffer bad_rx;
            bad_rx.open(*ctx, bad);
            malformed_rejected = false;
        } catch (const std::exception&) {
            // expected
        }
    }
    expect(malformed_rejected, "malformed identifiers are rejected");

    // Stale identifier: pid that does not exist.
    bool stale_rejected = false;
    try {
        RxSharedBuffer stale;
        stale.open(*ctx, L"gputxrx1:4194303:1");
    } catch (const std::exception& e) {
        stale_rejected = true;
        std::printf("       stale-identifier error: %s\n", e.what());
    }
    expect(stale_rejected, "stale identifier (dead pid) is rejected");

    std::printf("=== test_shared_resource %s (%d failures) ===\n", failures ? "FAILED" : "PASSED",
                failures);
    return failures ? 1 : 0;
}
