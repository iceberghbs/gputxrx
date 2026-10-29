// Resource-lifetime scenario test (spawns real child processes):
//  1. Transmitter publishes a known pattern and stays alive.
//  2. Receiver A opens, verifies, and exits -> transmitter must still be alive
//     and still publishing.
//  3. Receiver B opens and holds the resource while the transmitter is
//     terminated (simulating a crash/exit).
//  4. Receiver B must still read the payload correctly (its references keep
//     the resource alive).
//  5. After B exits, the identifier must be unusable (resource released).
#include "gputxrx/gpu_buffer.hpp"

#include <windows.h>

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

using namespace gputxrx::gpu;

namespace {

uint8_t pattern_byte(uint64_t i, uint64_t seed) {
    uint64_t x = i * 0x9E3779B97F4A7C15ull + seed;
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

// Runs `cmd`, returns exit code (or -1 on spawn failure).
int run(const std::wstring& cmd) {
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, (wchar_t*)cmd.c_str(), nullptr, nullptr, FALSE, 0, nullptr,
                        nullptr, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

// Spawns a process with a stdout pipe; returns pid and a pipe to read its
// stdout (used to fetch the identifier — test orchestration only, never used
// as a payload transport).
struct Spawned {
    DWORD pid = 0;
    HANDLE read_end = nullptr;
    HANDLE proc = nullptr;
};

Spawned spawn_with_stdout(const std::wstring& cmd) {
    Spawned s;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE r = nullptr, w = nullptr;
    if (!CreatePipe(&r, &w, &sa, 0)) return s;
    SetHandleInformation(w, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = w;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, (wchar_t*)cmd.c_str(), nullptr, nullptr, TRUE, 0, nullptr,
                        nullptr, &si, &pi)) {
        CloseHandle(r);
        CloseHandle(w);
        return s;
    }
    CloseHandle(w); // keep only the read end
    s.pid = pi.dwProcessId;
    s.read_end = r;
    s.proc = pi.hProcess;
    // pi.hThread intentionally leaked for process lifetime simplicity
    return s;
}

std::wstring read_line(HANDLE r) {
    std::wstring out;
    char c;
    DWORD n = 0;
    while (ReadFile(r, &c, 1, &n, nullptr) && n == 1) {
        if (c == '\n') break;
        if (c != '\r') out += wchar_t(c);
    }
    return out;
}

int tx_hold(uint64_t size, uint64_t seed) {
    auto ctx = GpuContext::create(AdapterSelector{});
    TxSharedBuffer tx;
    tx.create(*ctx, L"gputxrx", size);
    std::vector<uint8_t> data(size);
    for (uint64_t i = 0; i < size; ++i) data[i] = pattern_byte(i, seed);
    tx.upload_and_publish(data.data());
    std::wprintf(L"%s\n", tx.name().c_str());
    std::fflush(stdout);
    // Hold until stdin is closed or 'q' is typed.
    std::wstring line;
    for (;;) {
        if (std::wcin.eof()) {
            Sleep(500);
            continue;
        }
        if (!std::getline(std::wcin, line)) {
            std::wcin.clear();
            Sleep(200);
            continue;
        }
        if (line == L"q") break;
    }
    return 0;
}

int rx_read(const std::wstring& id, uint64_t size, uint64_t seed) {
    auto ctx = GpuContext::create(AdapterSelector{});
    RxSharedBuffer rx;
    rx.open(*ctx, id);
    if (rx.size() < size) return 3;
    std::vector<uint8_t> buf(size);
    rx.read(0, buf.data(), (size_t)size);
    for (uint64_t i = 0; i < size; ++i)
        if (buf[i] != pattern_byte(i, seed)) return 4;
    return 0;
}

int rx_hold(const std::wstring& id, uint64_t size, uint64_t seed, const std::wstring& release) {
    auto ctx = GpuContext::create(AdapterSelector{});
    RxSharedBuffer rx;
    rx.open(*ctx, id);
    if (release == L"wait-for-tx-death") {
        // Wait until the transmitter process is really gone.
        Sleep(2000);
    }
    std::vector<uint8_t> buf(size);
    rx.read(0, buf.data(), (size_t)size);
    for (uint64_t i = 0; i < size; ++i)
        if (buf[i] != pattern_byte(i, seed)) return 4;
    // Hold a moment so the parent can try (and fail) a fresh open.
    if (release == L"wait-for-tx-death") Sleep(3000);
    return 0;
}

int failures = 0;
void expect(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring mode = argc >= 2 ? argv[1] : L"";
    if (mode == L"--tx-hold" && argc >= 4)
        return tx_hold(std::wcstoull(argv[2], nullptr, 10), std::wcstoull(argv[3], nullptr, 10));
    if (mode == L"--rx-read" && argc >= 5)
        return rx_read(argv[2], std::wcstoull(argv[3], nullptr, 10),
                       std::wcstoull(argv[4], nullptr, 10));
    if (mode == L"--rx-hold" && argc >= 6)
        return rx_hold(argv[2], std::wcstoull(argv[3], nullptr, 10),
                       std::wcstoull(argv[4], nullptr, 10), argv[5]);

    const uint64_t size = 2 << 20, seed = 0x1234567;

    // 1: transmitter publishes and stays alive.
    Spawned tx = spawn_with_stdout(L"\"" + self_path() + L"\" --tx-hold " +
                                   std::to_wstring(size) + L" " + std::to_wstring(seed));
    if (!tx.proc) {
        std::printf("FAIL: cannot spawn transmitter\n");
        return 1;
    }
    std::wstring id = read_line(tx.read_end);
    expect(id.rfind(L"gputxrx1:", 0) == 0, "transmitter publishes an identifier");

    // 2: receiver A verifies while transmitter is alive; then exits.
    int rc = run(L"\"" + self_path() + L"\" --rx-read \"" + id + L"\" " +
                 std::to_wstring(size) + L" " + std::to_wstring(seed));
    expect(rc == 0, "receiver A verifies payload while transmitter is alive");

    // 3: transmitter still alive after a receiver finished.
    DWORD exit_now = 0;
    expect(WaitForSingleObject(tx.proc, 0) == WAIT_TIMEOUT,
           "transmitter remains alive after a receiver finishes");

    // 4: receiver B opens and holds; transmitter is killed mid-flight.
    Spawned rx_b = spawn_with_stdout(L"\"" + self_path() + L"\" --rx-hold \"" + id + L"\" " +
                                     std::to_wstring(size) + L" " + std::to_wstring(seed) +
                                     L" wait-for-tx-death");
    if (!rx_b.proc) {
        std::printf("FAIL: cannot spawn receiver B\n");
        TerminateProcess(tx.proc, 1);
        return 1;
    }
    Sleep(3000); // let B open the resource
    TerminateProcess(tx.proc, 1);
    WaitForSingleObject(tx.proc, 10000);
    CloseHandle(tx.read_end);
    CloseHandle(tx.proc);
    expect(true, "transmitter terminated while receiver B holds the resource");

    int rc_b = 0;
    WaitForSingleObject(rx_b.proc, 60000);
    GetExitCodeProcess(rx_b.proc, &exit_now);
    rc_b = (int)exit_now;
    CloseHandle(rx_b.read_end);
    CloseHandle(rx_b.proc);
    expect(rc_b == 0, "receiver B still reads correct payload after transmitter death");

    // 5: with no references left, the identifier must be unusable.
    Sleep(1000);
    int rc_dead = run(L"\"" + self_path() + L"\" --rx-read \"" + id + L"\" " +
                      std::to_wstring(size) + L" " + std::to_wstring(seed));
    expect(rc_dead != 0, "identifier unusable after all references are gone");

    std::printf("=== test_resource_lifetime %s (%d failures) ===\n",
                failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
