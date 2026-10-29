#pragma once
// GPU shared-buffer transport over Direct3D 12.
//
// Verified sharing mechanism (Milestone 0, tested on NVIDIA GT 1030, driver
// 560.94, Windows 11):
//   * The transmitter creates a committed resource with D3D12_HEAP_FLAG_SHARED
//     via ID3D12Device4::CreateCommittedResource1, buffer dimensions, flag
//     D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET (required by D3D12 for shareable
//     resources), initial state D3D12_RESOURCE_STATE_COMMON.
//   * It exports an NT handle with ID3D12Device::CreateSharedHandle
//     (Access = GENERIC_ALL, the only supported value). The NT handle is kept
//     open by the transmitter for its whole lifetime.
//   * ID3D12Device::OpenSharedHandleByName does NOT resolve names registered by
//     ID3D12Device::CreateSharedHandle on this system (verified: E_HANDLE even
//     while the publishing handle is open, even from another process). Named
//     D3D12 handles are therefore NOT used as the identifier.
//   * Instead the identifier is the manually copied string
//         gputxrx1:<tx_pid>:<nt_handle_value>
//     The receiver opens the transmitter process with PROCESS_DUP_HANDLE,
//     duplicates the NT handle with DuplicateHandle(DUPLICATE_SAME_ACCESS), and
//     opens it with ID3D12Device::OpenSharedHandle as an ID3D12Resource.
//     This is the documented D3D12 cross-process sharing mechanism and was
//     verified working between two independent processes on the target machine.
//     No sockets, pipes, brokers, or other IPC messaging channels are used.
//
// Lifetime: the kernel object lives as long as any reference exists (the
// transmitter's resource + shared NT handle, or any receiver's duplicated
// handle / opened resource). A receiver that has opened the buffer keeps it
// alive even after the transmitter exits. A transmitter exiting with no
// receivers releases the resource.

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

namespace gputxrx::gpu {

// Largest single GPU->CPU readback chunk; bounds readback-buffer VRAM/host cost.
inline constexpr uint64_t kMaxReadbackChunk = 64ull << 20; // 64 MiB

// Largest single CPU->GPU upload staging chunk (upload heap is host memory).
inline constexpr uint64_t kMaxUploadChunk = 256ull << 20; // 256 MiB

struct MemoryInfo {
    uint64_t budget = 0;         // bytes, DXGI local segment group budget
    uint64_t current_usage = 0;  // bytes, DXGI local segment group usage
};

struct AdapterInfo {
    std::wstring description;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    uint64_t dedicated_video_memory = 0;
    uint32_t luid_high = 0;
    uint32_t luid_low = 0;
    uint32_t feature_level = 0; // packed D3D_FEATURE_LEVEL of the created device
    MemoryInfo memory{};
};

// Adapter selection policy: NVIDIA hardware only. Never falls back to the
// integrated AMD adapter or to a software adapter.
struct AdapterSelector {
    uint32_t required_vendor_id = 0x10DE;      // NVIDIA PCI vendor id
    std::wstring name_hint = L"GeForce";       // preferred (not required) name match
};

class GpuContext {
public:
    // Enumerates adapters, selects the NVIDIA device, creates the D3D12 device,
    // queue, command allocator and fence. Throws std::runtime_error with an
    // actionable diagnostic if no suitable NVIDIA adapter exists.
    static std::unique_ptr<GpuContext> create(const AdapterSelector& selector);

    ~GpuContext();
    GpuContext(const GpuContext&) = delete;
    GpuContext& operator=(const GpuContext&) = delete;

    const AdapterInfo& adapter() const noexcept { return adapter_; }
    MemoryInfo memory_info() const;

    // Blocks until all GPU work submitted so far has completed.
    void flush_gpu();

    // Internal access for the shared-buffer classes.
    struct Impl;
    Impl& impl() noexcept { return *impl_; }

private:
    explicit GpuContext(std::unique_ptr<Impl> impl);
    struct Impl;
    std::unique_ptr<Impl> impl_;
    AdapterInfo adapter_;

    friend struct ImplAccess;
};

// Error type carrying an HRESULT-style diagnostic.
class GpuError : public std::runtime_error {
public:
    GpuError(const std::string& msg, long hr) : std::runtime_error(msg), hr_(hr) {}
    long hresult() const noexcept { return hr_; }

private:
    long hr_;
};

// Transmitter side: creates the shared resource, uploads the payload, publishes
// the name, and retains ownership until destruction / process exit.
class TxSharedBuffer {
public:
    TxSharedBuffer();
    ~TxSharedBuffer();
    TxSharedBuffer(const TxSharedBuffer&) = delete;
    TxSharedBuffer& operator=(const TxSharedBuffer&) = delete;

    // Creates a shared heap + placed buffer of `size` bytes on `ctx`'s device.
    // Does not publish yet.
    void create(GpuContext& ctx, const std::wstring& name, uint64_t size);

    // Copies `bytes` (exactly buffer size) into the GPU resource, waits until
    // the GPU work completes and the resource is fully visible, then exports
    // the named shared handle. Only after this returns is `name()` valid to
    // hand to a receiver.
    void upload_and_publish(const uint8_t* bytes);

    const std::wstring& name() const noexcept { return name_; }
    uint64_t size() const noexcept { return size_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::wstring name_;
    uint64_t size_ = 0;
};

// Receiver side: opens the shared resource read-only and copies data GPU->CPU.
// The application never writes through this resource.
class RxSharedBuffer {
public:
    RxSharedBuffer();
    ~RxSharedBuffer();
    RxSharedBuffer(const RxSharedBuffer&) = delete;
    RxSharedBuffer& operator=(const RxSharedBuffer&) = delete;

    // Opens by the identifier string published by the transmitter.
    void open(GpuContext& ctx, const std::wstring& name);

    // Trustworthy resource size from the driver (resource->GetDesc().Width).
    uint64_t size() const noexcept { return size_; }

    // Copies [offset, offset+len) into `out`. len must be <= kMaxReadbackChunk.
    void read(uint64_t offset, uint8_t* out, size_t len);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    uint64_t size_ = 0;
};

} // namespace gputxrx::gpu
