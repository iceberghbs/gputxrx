# AGENTS.md — guidance for AI agents and humans working on gputxrx

Read this before changing the design. Several things in this codebase look
"wrong" or suboptimal but are the **experimentally verified** result of
Milestone 0 on the target machine. Do not "simplify" them without re-running
the probes.

## Project summary

Native Windows C++20 CLI that transfers a directory between two local
processes through one shared D3D12 GPU buffer on the NVIDIA GPU (GT 1030 on
the dev machine). Hard constraints from the spec:

- No sockets, named pipes, brokers, or any other IPC messaging channel. The
  ONLY thing crossing processes is the manually copied identifier string.
- Exactly one logical GPU allocation holds the whole payload. No fallback
  transport, ever — reject with a diagnostic instead.
- Never silently fall back to the AMD iGPU or a software adapter.
- Transmitter stays alive indefinitely after publishing.

## Build & test commands

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64   # VS18 is what's installed
cmake --build build --config Release
# run all tests (all must exit 0):
build\Release\test_checksums.exe
build\Release\test_payload.exe
build\Release\test_shared_resource.exe
build\Release\test_gpu_roundtrip.exe
build\Release\test_resource_lifetime.exe
```

Note: the installed VS is "Visual Studio 18" (toolset v180, MSVC 14.51). The
`-G "Visual Studio 17 2022"` generator fails with MSB8020 — use the VS 18
generator. Console output on this machine is partially Chinese (system
locale); don't be confused by garbled MSBuild messages.

When running console programs through PowerShell, stderr becomes noisy
`NativeCommandError` noise — prefer `cmd /c "... 2>&1"` and check
`$LASTEXITCODE`.

## The verified sharing mechanism — do not casually change

`include/gputxrx/gpu_buffer.hpp` (header comment) and README document the
mechanism. Facts established by experiment on this machine (driver 560.94,
Win 11):

1. `ID3D12Device::OpenSharedHandleByName` **cannot** resolve names registered
   by `ID3D12Device::CreateSharedHandle`:
   - `Access` other than `GENERIC_ALL` → `E_INVALIDARG`.
   - `GENERIC_ALL` → `E_HANDLE` even while the publishing handle is open,
     even from another process, even same-process.
   - Registering via `IDXGIResource1::CreateSharedHandle` is impossible:
     D3D12 buffers do not expose `IDXGIResource1` (`E_NOINTERFACE`).
2. `CreateSharedHandle` on a **placed** resource on a shared heap fails with
   `E_INVALIDARG` (named and unnamed). Creating the resource as a **committed
   shared resource** via `ID3D12Device4::CreateCommittedResource1` works.
3. `D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS` on a buffer →
   `E_INVALIDARG` from `CreateCommittedResource1`. Only
   `ALLOW_RENDER_TARGET` is used.
4. Cross-process `DuplicateHandle(PROCESS_DUP_HANDLE)` + `OpenSharedHandle`
   works reliably; this is what the identifier `<tx_pid>:<handle>` drives.
   All of this is exercised by `test_gpu_roundtrip`.

If you want to revisit named handles, write a scratch probe first and only
change the design if it works cross-process on the target machine.

## Known D3D12 / Win32 gotchas already hit (don't re-introduce)

- **Fences must be signaled by the queue**: use `queue->Signal(fence, v)`,
  never `fence->Signal(fence, v)`. The latter advances immediately from the
  CPU and every wait becomes a no-op — this produced a silent data race
  (stale readback chunks) during development.
- **Upload/readback visibility**: `Map` after a GPU copy only returns fresh
  data if the queue fence actually waited (see above).
- **pimpl with `std::unique_ptr<Impl>`**: MSVC instantiates the unique_ptr
  destructor in every TU unless the enclosing class's *default constructor
  and destructor are both defined out-of-line* in the backend .cpp
  (`GpuContext`, `TxSharedBuffer`, `RxSharedBuffer` all follow this).
  Inlining `= default` in the header breaks builds with
  "can't delete an incomplete type".
- Buffers need no resource barriers for copies (implicit COMMON
  promote/decay), but the shared resource is kept in `D3D12_RESOURCE_STATE_COMMON`
  and explicit transitions are used on the upload path for clarity.
- `GetEnvironmentVariableW`/`swscanf_s` (Unicode) — the project compiles with
  `UNICODE`; don't mix A/W variants.

## C++ gotchas hit in this codebase

- Most vexing parse: `std::vector<uint8_t> buf(size_t(n));` declares a
  *function*. Use `std::vector<uint8_t> buf(size_t(n), 0)`.
- `std::filesystem::rename/ remove/ remove_all` (error_code overloads) return
  **void** — check `ec`, not the return value.
- CRC-32 of the payload is defined over the final buffer with the header CRC
  field zeroed. The transmitter must therefore write the header (crc=0)
  first, compute the CRC, then patch the field
  (`put_u32(buf + kCrcFieldOffset, ...)`). The receiver zeroes only the crc
  field bytes and must count the header region exactly once (see `run_rx`).

## Architecture map

```
include/gputxrx/
  cli.hpp        argument parsing entry (int run(argc, argv))
  payload.hpp    wire format: constants, Header/Entry/Layout, plan/parse/write,
                 path safety (validate_payload_path)
  gpu_buffer.hpp GPU API surface: GpuContext / TxSharedBuffer / RxSharedBuffer,
                 limits (kMaxReadbackChunk 64 MiB, kMaxUploadChunk 256 MiB)
  checksum.hpp   Md5 (streaming) + crc32; modular so SHA-256 can be added
  storage.hpp    filesystem enumeration (collect_files), staging extraction
                 (stage_payload / commit_staged / check_conflicts)
src/
  main.cpp       wmain -> cli::run, top-level error handler
  cli.cpp        run_tx / run_rx orchestration, budget precheck, timings,
                 transmitter keep-alive loop
  payload.cpp    format encode/decode + strict validation
  storage.cpp    enumeration, staging, MD5-verified writes, commit + rollback
  backends/d3d12_backend.cpp  all D3D12/DXGI code
tests/           see README table; test_* are standalone executables
```

Data flow, transmitter: `collect_files` (directory) or a single-file record
→ top-level root-name prefix on every payload path → `plan_layout` → budget
precheck →
serialize into one RAM buffer (MD5 per file while copying) → write manifest +
header → CRC-32 → `TxSharedBuffer::create/upload_and_publish` → print
identifier → keep-alive loop.

Data flow, receiver: `RxSharedBuffer::open(identifier)` → read+validate header
→ read+validate manifest (`parse_manifest` does bounds/overlap/dup/path
checks) → `check_conflicts` → `stage_payload` (MD5 per file, streaming 1 MiB
chunks, CRC accumulation) → CRC compare → `commit_staged`.

## Invariants to preserve when modifying

- Receiver-side validation is strict and never trusts the header/manifest:
  every offset/size is bounds-checked with checked arithmetic; all values are
  validated against the *driver-reported* resource size (`GetDesc().Width`).
- Payload paths are validated on both ends
  (`payload::validate_payload_path`); extraction must never escape the output
  root.
- The transmitter prints the identifier only after the fence wait confirms
  the GPU copy is complete.
- Enumeration must include hidden files and dot-directories and must reject
  symlinks/reparse points with a clear error (never skip, never follow).
- `tx` roots every payload path at the input's own name (directory or single
  file), so the receiver recreates the top-level entry. Inputs whose name
  cannot be determined (`.`/`..`, drive roots) are rejected with a clear
  error; every prefixed path is re-validated with
  `payload::validate_payload_path`.
- Receiver never overwrites existing files; never leaves staging dirs behind
  (RAII `StagedExtraction`).
- Errors: throw std::runtime_error-derived types; `main`/`wmain` prints
  `gputxrx: error: <what>` and exits 1 (2 for usage errors).

## Performance notes

Measured on the dev machine (see README table). Upload ~1.4 GiB/s,
readback ~259 MiB/s on the GT 1030. If improving: the transmitter currently
stages the full payload in RAM; chunked RAM staging plus progressive upload
would halve peak host memory at some throughput cost. Don't break the
"one logical GPU allocation" constraint doing it.

## Housekeeping

- `build/` is not tracked; remove stale `probe_out.txt`-style scratch files
  from the repo root before finishing.
- Keep `README.md` in sync with any format/CLI/mechanism change; it documents
  the exact verified mechanism and measured numbers.
- Test payloads live under `%TEMP%\gputxrx-e2e` (not in the repo).
