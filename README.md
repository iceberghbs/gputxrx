# gputxrx

`gputxrx` is a native Windows C++ command-line utility that transfers a
directory of files between two local processes through a shared GPU resource
(Direct3D 12, NVIDIA). There are no sockets, named pipes, brokers, or any
other IPC messaging channel: the transmitter publishes a shared GPU buffer,
prints a single identifier line, and the user copies that line into the
receiver command.

```
Terminal A:  gputxrx tx <payload_dir|payload_file>  -> prints  gputxrx1:<pid>:<handle>
Terminal B:  gputxrx rx 'gputxrx1:<pid>:<handle>' [output_dir]
```

`tx` accepts a directory or a single file of any type. Payload paths are
rooted at the input's own name: passing `.\hello\` makes the receiver
recreate `hello/...` under the output directory; passing `.\hello.7z`
extracts a file named `hello.7z`.

## Hardware / OS requirements

- Windows 10 1809+ / Windows 11 (uses `ID3D12Device4::CreateCommittedResource1`
  with shared heap flags and `DuplicateHandle`).
- An **NVIDIA** GPU with a driver that supports D3D12 shared committed
  resources. Developed and verified on a GeForce GT 1030 (Pascal, driver
  560.94) on Windows 11.
- The AMD integrated GPU and software adapters are never selected; if no
  suitable NVIDIA adapter exists, both `tx` and `rx` fail with an actionable
  diagnostic instead of falling back.
- Git Bash works for both processes (the executables are plain native Windows
  console programs).

## Build

```bash
cmake -S . -B build -G "Visual Studio 18 2026" -A x64   # or your VS generator
cmake --build build --config Release
# binaries in build/Release/
```

## Install (optional)

Double-click `install\install.bat` to build (if needed), copy
`gputxrx.exe` to `install\bin\`, and add it to the user `PATH` so
`gputxrx` works in any new terminal. See `install\README.md`;
`install\uninstall.bat` reverts it.


## Usage (Git Bash)

Terminal 1 — transmitter:

```bash
./build/Release/gputxrx.exe tx /d/some/payload_dir
# gputxrx tx: enumerating D:/some/payload_dir
#   files: 123
#   payload: 117441701 bytes (manifest 994, data 117440515)
#   GPU: NVIDIA GeForce GT 1030 budget=3407.7 MiB usage=10.9 MiB
#   limits: app max 2048.0 MiB, margin 256.0 MiB, safe limit 2048.0 MiB
#
# ================ COPY THIS IDENTIFIER ================
# gputxrx1:39184:3016
# =======================================================
#
# Timings: enumerate 1 ms, hash+serialize 350 ms ...
#          GPU upload 79 ms (1408.9 MiB/s)
# (stays alive until Ctrl+C or 'q' + Enter)
```

Terminal 2 — receiver:

```bash
./build/Release/gputxrx.exe rx 'gputxrx1:39184:3016' /d/some/output_dir
# output_dir defaults to the receiver's current working directory
```

Options:

- `tx --max-payload-mb N` — application-level payload cap (default 2048 MiB;
  env `GPUTXRX_MAX_PAYLOAD_MB`).
- `tx --margin-mb N` — VRAM safety margin kept unallocated (default 256 MiB;
  env `GPUTXRX_SAFETY_MARGIN_MB`).

What is transferred: every regular file under the payload directory,
recursively — including hidden files, empty files, and dot-directories such as
`.git`, `.github`, `.gitignore`. Relative paths and exact bytes are preserved
(UTF-8 paths). Symbolic links and other reparse points are rejected with a
clear error, never skipped or followed. Timestamps/permissions/alternate data
streams are not preserved.

## Verified resource-sharing mechanism

This is the exact mechanism, verified on the target machine (GT 1030,
driver 560.94, Windows 11) by `test_gpu_roundtrip` (Milestone 0 probe) and
`test_resource_lifetime`:

1. The transmitter creates a **committed shared resource** with
   `ID3D12Device4::CreateCommittedResource1` using `D3D12_HEAP_FLAG_SHARED`,
   buffer dimensions, `D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET` (required by
   D3D12 for shareable resources), initial state
   `D3D12_RESOURCE_STATE_COMMON`.
2. It exports an NT handle with `ID3D12Device::CreateSharedHandle`
   (`Access = GENERIC_ALL`, the only supported value) and keeps it open.
   Payload data is uploaded through an UPLOAD-heap staging buffer with
   `CopyBufferRegion`; a **command-queue fence** (`ID3D12CommandQueue::Signal`
   + `SetEventOnCompletion` wait) guarantees the data is fully written and
   visible before the identifier is published.
3. The identifier is the manually copied string `gputxrx1:<tx_pid>:<handle>`.
4. The receiver parses the identifier, opens the transmitter process with
   `OpenProcess(PROCESS_DUP_HANDLE)`, duplicates the handle with
   `DuplicateHandle(DUPLICATE_SAME_ACCESS)`, and opens it as an
   `ID3D12Resource` via `ID3D12Device::OpenSharedHandle` on its own NVIDIA
   device. It then copies data into its own READBACK-heap buffer (bounded
   64 MiB chunks) and maps/reads it.

**Why not named handles?** `ID3D12Device::OpenSharedHandleByName` cannot
resolve names registered by `ID3D12Device::CreateSharedHandle` on this
system: it returns `E_INVALIDARG` for any access value other than
`GENERIC_ALL`, and `E_HANDLE` ("not found") for `GENERIC_ALL` — even while
the publishing handle is held open, and even from another process. Named
D3D12 handles were therefore rejected after explicit experimentation (see
`AGENTS.md`). The handle-duplication path above is the documented D3D12
cross-process sharing mechanism and requires no IPC channel — the only thing
crossing the process boundary is the identifier string that the user copies.

## Resource lifetime semantics

- The transmitter owns the resource and keeps it published indefinitely while
  alive. A receiver finishing extraction does **not** stop the transmitter.
- If a receiver crashes, the transmitter keeps the resource.
- If the transmitter exits or crashes, its handles close; the kernel object is
  released once no references remain. A receiver that already opened the
  buffer keeps it alive (and readable) beyond transmitter death — verified by
  `test_resource_lifetime`. A *new* receiver cannot attach after transmitter
  death (`DuplicateHandle` needs a live transmitter process).
- There are no timeouts or expiration rules.
- Lifetime is governed by OS/GPU-resource references, not by whether a
  receiver has finished extracting.

## Payload format

One contiguous GPU buffer: `[header][manifest entries][path blob][file data]`.
All integers little-endian; paths UTF-8 with `/` separators; no alignment
padding.

Header (192 bytes): magic `GPXR`, format version (1), header size,
manifest entry size (56), 16-byte transfer id, total size, manifest offset
(always 192), manifest size, file count, data region offset/size,
whole-payload CRC-32 (over the entire buffer with the CRC field itself zeroed),
flags (0), reserved bytes (must be zero).

Manifest entry (56 bytes): path offset (relative to the path blob), path
length, reserved, file size, absolute data offset, 16-byte per-file MD5.

Limits and validation (enforced on the receive side, never trusted):
format caps of 1M files, 4096-byte paths, 512 MiB manifests, 64 GiB per file,
1 TiB total; strict contiguity of file data regions; duplicate-path rejection;
unsafe-path rejection (absolute, drive-qualified, `\` separators, `.`/`..`
components, empty components, control characters, trailing dot/space,
reserved device names); nonzero reserved bytes rejected. MD5 and CRC-32 guard
against accidental corruption only — not adversarial tampering. The checksum
implementation is modular (`include/gputxrx/checksum.hpp`), so a SHA-256 could
be added behind the same interface.

## Memory budget

Before any GPU allocation the transmitter computes the exact serialized size,
queries `IDXGIAdapter3::QueryVideoMemoryInfo` (local segment group), and
rejects the transfer if

```
payload > min(budget - current_usage - safety_margin, app_max)
```

Defaults: margin 256 MiB, app max 2048 MiB (both configurable as shown above).
The check happens *before* allocation; allocation failure is still handled and
cleaned up. **There is no fallback transport** — payloads that do not fit are
rejected with a diagnostic that prints payload size, budget, usage, and
limits.

Host memory: the transmitter stages the whole serialized payload in RAM once
(plus a bounded 256 MiB upload chunk); the receiver uses bounded 64 MiB
readback chunks plus a RAM copy of header+manifest.

## Extraction and safety

- Files are written into a staging directory
  (`<output>/.gputxrx-stage-<pid>-<tick>`), per-file MD5 is verified while
  writing, and the whole-payload CRC-32 must match before anything is
  committed.
- Existing destination files are never overwritten (conflicts are detected
  before staging and again at commit; the receiver refuses with a list).
- Commit is a sequence of renames — **atomicity of the whole tree is not
  guaranteed** by ordinary filesystem operations. On commit failure the
  receiver removes already-moved files on a best-effort basis and reports what
  happened; on any earlier failure the staging directory is removed entirely.
- Nothing from the payload is ever executed.

## Measured performance (GT 1030, Windows 11, ~112 MiB / 15-file payload)

| Stage | Time | Throughput |
|---|---|---|
| Directory enumeration | ~1 ms | — |
| Hash + serialize (incl. file read) | 350 ms (read 170 ms) | ~335 MiB/s disk |
| GPU upload | 79 ms | **1408.9 MiB/s** |
| GPU readback + verify | 433 ms | **258.8 MiB/s** |
| Staging + commit (disk write) | ~150 ms | ~750 MiB/s |
| End-to-end receiver total | ~0.6 s | ~190 MiB/s |

GPU transfer throughput is not the same as end-to-end file throughput: the
workflow includes filesystem reads, GPU upload, GPU readback, MD5/CRC
computation, and filesystem writes. The system is **not** zero-copy.

## Failure behavior

Clear diagnostics for: invalid CLI arguments; missing/unreadable directories;
symlinks/reparse points; files changing during enumeration or read; invalid,
stale, or inaccessible identifiers (including "transmitter has exited");
unsupported adapter/sharing configuration; GPU allocation failure; payload
over limit/budget; malformed header/manifest; overflow/out-of-bounds;
path traversal; MD5/CRC mismatch; existing destination files; disk-full and
filesystem errors. GPU adapter selection and key payload sizes are logged to
stderr (prefix `[gputxrx-gpu]`). Set `GPUTXRX_D3D12_DEBUG=1` to enable the
D3D12 debug layer for deeper diagnostics.

## Tests

| Executable | Verifies |
|---|---|
| `test_gpu_roundtrip` | Milestone 0 probe: adapter selection, cross-process open by identifier, byte-pattern round-trip, fence visibility, invalid identifier, release after termination |
| `test_shared_resource` | Same-process open, malformed/stale identifier rejection |
| `test_resource_lifetime` | Multi-process lifetime: tx alive after receiver finishes; receiver holds resource past transmitter death; identifier dead after all references gone |
| `test_checksums` | MD5/CRC-32 known vectors, streaming == one-shot |
| `test_payload` | Layout arithmetic, header/manifest round-trip, tamper detection, unsafe paths, duplicates, overlap, truncation, reserved bytes |

Run each from `build/Release/`; all print `PASSED` and exit 0 on success.

## Known limitations

- One outstanding transfer per transmitter; any number of receivers may open
  the same published resource read-only (they must not modify it).
- Entire payload must fit in one GPU allocation (by design — no chunked
  transfer, no fallback).
- Tree commit is not atomic; see above.
- Timestamps, permissions, and alternate data streams are not preserved.
- The identifier contains the transmitter PID and NT handle value; treat it
  like a capability: anyone who can copy it and runs as the same user can
  attach a receiver. This tool is for local, same-user transfers.
