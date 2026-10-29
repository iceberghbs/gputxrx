#include "gputxrx/gpu_buffer.hpp"

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace gputxrx::gpu {

namespace {

std::string hex_hr(HRESULT hr) {
    std::ostringstream os;
    os << "0x" << std::uppercase << std::hex << static_cast<unsigned long>(hr);
    return os.str();
}

[[noreturn]] void fail_hr(HRESULT hr, const char* api, const char* extra = nullptr) {
    std::ostringstream os;
    os << api << " failed (HRESULT " << hex_hr(hr) << ")";
    if (extra) os << ": " << extra;
    throw GpuError(os.str(), hr);
}

void check_hr(HRESULT hr, const char* api, const char* extra = nullptr) {
    if (FAILED(hr)) fail_hr(hr, api, extra);
}

void log_line(const char* tag, const std::string& msg) {
    std::fprintf(stderr, "[gputxrx-gpu] %s: %s\n", tag, msg.c_str());
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    std::string out(w.size() * 4, '\0');
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), out.data(), (int)out.size(),
                                nullptr, nullptr);
    if (n <= 0) return {};
    out.resize((size_t)n);
    return out;
}

std::wstring feature_level_string(UINT fl) {
    std::wostringstream os;
    os << (fl >> 12) << L'_' << ((fl >> 8) & 0xF);
    return os.str();
}

uint64_t round_up_64(uint64_t v, uint64_t align) {
    return (v + align - 1) / align * align;
}

} // namespace

// ---------------------------------------------------------------------------
// GpuContext::Impl
// ---------------------------------------------------------------------------

struct GpuContext::Impl {
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter3> adapter3;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;
    uint64_t fence_value = 0;
    bool debug_layer_enabled = false;

    ~Impl() {
        if (fence_event) CloseHandle(fence_event);
    }

    // Resets the command allocator, returns an open (recording) command list.
    ComPtr<ID3D12GraphicsCommandList> begin_list() {
        check_hr(allocator->Reset(), "ID3D12CommandAllocator::Reset");
        ComPtr<ID3D12GraphicsCommandList> list;
        check_hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                           nullptr, IID_PPV_ARGS(&list)),
                 "ID3D12Device::CreateCommandList");
        return list; // created in recording state
    }

    void submit_and_wait(ID3D12GraphicsCommandList* list) {
        check_hr(list->Close(), "ID3D12GraphicsCommandList::Close");
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        ++fence_value;
        // Must signal via the QUEUE so the fence advances only when the GPU has
        // finished executing; ID3D12Fence::Signal would advance it immediately
        // from the CPU and every wait would be a no-op.
        check_hr(queue->Signal(fence.Get(), fence_value), "ID3D12CommandQueue::Signal");
        if (fence->GetCompletedValue() < fence_value) {
            check_hr(fence->SetEventOnCompletion(fence_value, fence_event),
                     "ID3D12Fence::SetEventOnCompletion");
            DWORD w = WaitForSingleObject(fence_event, INFINITE);
            if (w != WAIT_OBJECT_0)
                throw GpuError("WaitForSingleObject(fence) failed; GPU work did not complete", -1);
        }
    }
};

GpuContext::GpuContext(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

GpuContext::~GpuContext() = default;

std::unique_ptr<GpuContext> GpuContext::create(const AdapterSelector& selector) {
    auto impl = std::make_unique<Impl>();

    // Optional debug layer for diagnostics (set GPUTXRX_D3D12_DEBUG=1).
    char flag[2] = {0, 0};
    if (GetEnvironmentVariableA("GPUTXRX_D3D12_DEBUG", flag, 2) == 1 && flag[0] == '1') {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) {
            dbg->EnableDebugLayer();
            impl->debug_layer_enabled = true;
            log_line("info", "D3D12 debug layer enabled");
        }
    }

    check_hr(CreateDXGIFactory1(IID_PPV_ARGS(&impl->factory)), "CreateDXGIFactory1");

    // Enumerate adapters: keep NVIDIA hardware adapters only.
    struct Candidate {
        ComPtr<IDXGIAdapter1> adapter;
        DXGI_ADAPTER_DESC1 desc;
        bool hint_match;
    };
    std::vector<Candidate> candidates;
    std::vector<std::wstring> seen_names;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> a;
        HRESULT hr = impl->factory->EnumAdapters1(i, &a);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        check_hr(hr, "IDXGIFactory4::EnumAdapters1");

        DXGI_ADAPTER_DESC1 desc{};
        check_hr(a->GetDesc1(&desc), "IDXGIAdapter1::GetDesc1");
        seen_names.push_back(desc.Description);

        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;     // never software
        if (desc.VendorId != selector.required_vendor_id) continue; // never non-NVIDIA
        bool hint = std::wstring(desc.Description).find(selector.name_hint) != std::wstring::npos;
        candidates.push_back({std::move(a), desc, hint});
    }

    if (candidates.empty()) {
        std::ostringstream os;
        os << "no NVIDIA hardware adapter found (vendor id 0x"
           << std::hex << selector.required_vendor_id << "); adapters seen: ";
        for (size_t i = 0; i < seen_names.size(); ++i) {
            if (i) os << ", ";
            os << '"' << narrow(seen_names[i]) << '"';
        }
        os << ". Refusing to fall back to a non-NVIDIA adapter.";
        throw GpuError(os.str(), -1);
    }

    // Prefer an adapter matching the name hint, otherwise the one with the most
    // dedicated video memory. Log every NVIDIA adapter seen.
    for (auto& c : candidates) {
        log_line("info", "NVIDIA adapter: " + narrow(c.desc.Description) +
                             " (vid=0x" + [&] {
                                 std::ostringstream t;
                                 t << std::hex << c.desc.VendorId;
                                 return t.str();
                             }() +
                             ", did=0x" + [&] {
                                 std::ostringstream t;
                                 t << std::hex << c.desc.DeviceId;
                                 return t.str();
                             }() +
                             ", dedicated=" + std::to_string(c.desc.DedicatedVideoMemory) +
                             " bytes" + (c.hint_match ? ", matches hint" : "") + ")");
    }
    Candidate* chosen = nullptr;
    for (auto& c : candidates)
        if (c.hint_match && (!chosen || c.desc.DedicatedVideoMemory > chosen->desc.DedicatedVideoMemory))
            chosen = &c;
    if (!chosen) {
        for (auto& c : candidates)
            if (!chosen || c.desc.DedicatedVideoMemory > chosen->desc.DedicatedVideoMemory)
                chosen = &c;
    }

    check_hr(chosen->adapter.As(&impl->adapter3), "IDXGIAdapter1::QueryInterface(IDXGIAdapter3)");

    // Create the device, preferring the highest supported feature level.
    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0};
    HRESULT last_hr = E_FAIL;
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
    for (D3D_FEATURE_LEVEL fl : levels) {
        last_hr = D3D12CreateDevice(chosen->adapter.Get(), fl, IID_PPV_ARGS(&impl->device));
        if (SUCCEEDED(last_hr)) {
            got = fl;
            break;
        }
    }
    if (FAILED(last_hr))
        fail_hr(last_hr, "D3D12CreateDevice",
                ("on adapter '" + narrow(chosen->desc.Description) + "'").c_str());

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check_hr(impl->device->CreateCommandQueue(&qd, IID_PPV_ARGS(&impl->queue)),
             "ID3D12Device::CreateCommandQueue");
    check_hr(impl->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&impl->allocator)),
             "ID3D12Device::CreateCommandAllocator");
    check_hr(impl->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&impl->fence)),
             "ID3D12Device::CreateFence");
    impl->fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!impl->fence_event) throw GpuError("CreateEventW failed for fence event", -1);

    auto ctx = std::unique_ptr<GpuContext>(new GpuContext(std::move(impl)));
    AdapterInfo& info = ctx->adapter_;
    info.description = chosen->desc.Description;
    info.vendor_id = chosen->desc.VendorId;
    info.device_id = chosen->desc.DeviceId;
    info.dedicated_video_memory = chosen->desc.DedicatedVideoMemory;
    info.luid_high = chosen->desc.AdapterLuid.HighPart;
    info.luid_low = chosen->desc.AdapterLuid.LowPart;
    info.feature_level = got;
    info.memory = ctx->memory_info();

    log_line("info", "selected adapter: \"" + narrow(info.description) +
                         "\" feature level " + narrow(feature_level_string(got)));
    return ctx;
}

MemoryInfo GpuContext::memory_info() const {
    DXGI_QUERY_VIDEO_MEMORY_INFO q{};
    HRESULT hr = impl_->adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &q);
    if (FAILED(hr)) {
        log_line("warn", "QueryVideoMemoryInfo failed: " + hex_hr(hr));
        return {};
    }
    return {q.Budget, q.CurrentUsage};
}

void GpuContext::flush_gpu() {
    // All work is submitted with inline waits; a flush only needs the fence to
    // catch up with work already submitted on the queue.
    auto* impl = impl_.get();
    ++impl->fence_value;
    check_hr(impl->queue->Signal(impl->fence.Get(), impl->fence_value),
             "ID3D12CommandQueue::Signal");
    if (impl->fence->GetCompletedValue() < impl->fence_value) {
        check_hr(impl->fence->SetEventOnCompletion(impl->fence_value, impl->fence_event),
                 "ID3D12Fence::SetEventOnCompletion");
        WaitForSingleObject(impl->fence_event, INFINITE);
    }
}

// ---------------------------------------------------------------------------
// TxSharedBuffer
// ---------------------------------------------------------------------------

struct TxSharedBuffer::Impl {
    GpuContext::Impl* gpu = nullptr;
    ComPtr<ID3D12Resource> resource;
    ComPtr<ID3D12Resource> upload; // staging, UPLOAD heap, persistently mapped
    uint8_t* upload_mapped = nullptr;
    HANDLE shared_handle = nullptr; // kept open for the transmitter's lifetime
    uint64_t size = 0;
    bool published = false;

    ~Impl() {
        if (shared_handle) CloseHandle(shared_handle);
        if (upload_mapped && upload) upload->Unmap(0, nullptr);
    }
};

TxSharedBuffer::TxSharedBuffer() = default;
TxSharedBuffer::~TxSharedBuffer() = default;

void TxSharedBuffer::create(GpuContext& ctx, const std::wstring& resource_name, uint64_t size) {
    (void)resource_name; // the identifier is generated at publish time
    if (impl_) throw GpuError("TxSharedBuffer already created", -1);
    if (size == 0) throw GpuError("cannot create a zero-byte shared buffer", -1);

    ComPtr<ID3D12Device4> dev4;
    check_hr(ctx.impl().device.As(&dev4), "ID3D12Device::QueryInterface(ID3D12Device4)",
             "ID3D12Device4 (Windows 10 1809+) is required for committed shared resources");

    impl_ = std::make_unique<Impl>();
    impl_->gpu = &ctx.impl();
    impl_->size = size;
    size_ = size;

    ID3D12Device* dev = impl_->gpu->device.Get();

    D3D12_HEAP_PROPERTIES default_props{};
    default_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC res_desc{};
    res_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    res_desc.Alignment = 0;
    res_desc.Width = size;
    res_desc.Height = 1;
    res_desc.DepthOrArraySize = 1;
    res_desc.MipLevels = 1;
    res_desc.Format = DXGI_FORMAT_UNKNOWN;
    res_desc.SampleDesc.Count = 1;
    res_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    // ALLOW_RENDER_TARGET is required by D3D12 for a shareable resource.
    res_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    check_hr(dev4->CreateCommittedResource1(&default_props, D3D12_HEAP_FLAG_SHARED, &res_desc,
                                            D3D12_RESOURCE_STATE_COMMON, nullptr, nullptr,
                                            IID_PPV_ARGS(&impl_->resource)),
             "ID3D12Device4::CreateCommittedResource1(D3D12_HEAP_FLAG_SHARED)",
             "shared committed buffers require driver support");

    D3D12_HEAP_PROPERTIES upload_props{};
    upload_props.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC up_desc = res_desc;
    up_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    check_hr(dev->CreateCommittedResource(&upload_props, D3D12_HEAP_FLAG_NONE, &up_desc,
                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                          IID_PPV_ARGS(&impl_->upload)),
             "ID3D12Device::CreateCommittedResource(upload)");
    D3D12_RANGE no_read{0, 0};
    check_hr(impl_->upload->Map(0, &no_read, reinterpret_cast<void**>(&impl_->upload_mapped)),
             "ID3D12Resource::Map(upload)");
}

void TxSharedBuffer::upload_and_publish(const uint8_t* bytes, const ProgressFn& progress) {
    if (!impl_ || !impl_->resource) throw GpuError("TxSharedBuffer::create was not called", -1);
    if (impl_->published) throw GpuError("buffer already published", -1);

    auto* gpu = impl_->gpu;
    ID3D12Device* dev = gpu->device.Get();

    // Upload in bounded staging chunks. A single large copy command list would
    // require an upload buffer as big as the payload; chunking bounds host
    // memory while the shared resource itself stays one logical allocation.
    ComPtr<ID3D12Resource> chunk;
    const uint64_t chunk_size =
        std::min<uint64_t>(kMaxUploadChunk, round_up_64(impl_->size, 65536));
    {
        D3D12_HEAP_PROPERTIES up{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC cd{};
        cd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        cd.Width = chunk_size;
        cd.Height = 1;
        cd.DepthOrArraySize = 1;
        cd.MipLevels = 1;
        cd.SampleDesc.Count = 1;
        cd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check_hr(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &cd,
                                              D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                              IID_PPV_ARGS(&chunk)),
                 "ID3D12Device::CreateCommittedResource(upload chunk)");
    }
    uint8_t* mapped = nullptr;
    {
        D3D12_RANGE no_read{0, 0};
        check_hr(chunk->Map(0, &no_read, reinterpret_cast<void**>(&mapped)),
                 "ID3D12Resource::Map(chunk)");
    }

    uint64_t off = 0;
    while (off < impl_->size) {
        const uint64_t n = std::min<uint64_t>(chunk_size, impl_->size - off);
        memcpy(mapped, bytes + off, (size_t)n);
        auto list = gpu->begin_list();
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = impl_->resource.Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &b);
        list->ResourceBarrier(1, &b);
        list->CopyBufferRegion(impl_->resource.Get(), off, chunk.Get(), 0, n);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        list->ResourceBarrier(1, &b);
        gpu->submit_and_wait(list.Get());
        off += n;
        if (progress) progress(off, impl_->size);
    }
    D3D12_RANGE written{0, (SIZE_T)0};
    chunk->Unmap(0, &written);
    chunk.Reset();

    // The resource is fully written, the GPU work is complete (fence waited),
    // and the resource is in COMMON state: export the NT handle now.
    // Access must be GENERIC_ALL (the only supported value). A name is used so
    // the handle export path matches the verified Milestone 0 configuration;
    // the name itself is NOT the identifier (OpenSharedHandleByName cannot
    // resolve D3D12-registered names on this system).
    std::wstring kernel_name = L"Global\\gputxrx-tx-" + std::to_wstring(GetCurrentProcessId());
    check_hr(dev->CreateSharedHandle(impl_->resource.Get(), nullptr, GENERIC_ALL,
                                     kernel_name.c_str(), &impl_->shared_handle),
             "ID3D12Device::CreateSharedHandle",
             "shared NT handle export requires driver support");

    // Publish the identifier: process id + NT handle value. The receiver
    // duplicates this handle from our process (DuplicateHandle) and opens it.
    impl_->published = true;
    name_ = std::to_wstring(GetCurrentProcessId()) + L":" +
            std::to_wstring((unsigned long long)(uintptr_t)impl_->shared_handle);
    log_line("info", "shared resource created and published (" +
                         std::to_string(impl_->size) + " bytes)");
}

// ---------------------------------------------------------------------------
// RxSharedBuffer
// ---------------------------------------------------------------------------

struct RxSharedBuffer::Impl {
    GpuContext::Impl* gpu = nullptr;
    ComPtr<ID3D12Resource> resource;
    ComPtr<ID3D12Resource> readback;
    uint8_t* readback_mapped = nullptr;
    uint64_t readback_size = 0;
    uint64_t size = 0;

    ~Impl() {
        if (readback_mapped && readback) readback->Unmap(0, nullptr);
    }
};

RxSharedBuffer::RxSharedBuffer() = default;
RxSharedBuffer::~RxSharedBuffer() = default;

void RxSharedBuffer::open(GpuContext& ctx, const std::wstring& identifier) {
    if (impl_) throw GpuError("RxSharedBuffer already opened", -1);

    // Identifier format: <tx_pid>:<nt_handle_value>. The legacy
    // "gputxrx1:"-prefixed form is rejected with an actionable message.
    if (identifier.rfind(L"gputxrx1:", 0) == 0)
        throw GpuError("invalid identifier: the legacy \"gputxrx1:\" prefix is no longer "
                       "supported; use \"<tx_pid>:<handle>\" exactly as printed by the "
                       "transmitter",
                       -1);
    size_t sep = identifier.find(L':');
    if (sep == std::wstring::npos)
        throw GpuError("invalid identifier: expected format \"<tx_pid>:<handle>\" "
                       "(copy it exactly as printed by the transmitter)",
                       -1);
    DWORD pid = 0;
    unsigned long long handle_value = 0;
    try {
        size_t idx = 0;
        pid = std::stoul(identifier.substr(0, sep), &idx);
        handle_value = std::stoull(identifier.substr(sep + 1), &idx, 10);
    } catch (const std::exception&) {
        throw GpuError("invalid identifier: pid/handle are not valid numbers", -1);
    }

    auto* gpu = &ctx.impl();

    HANDLE tx_proc = OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid);
    if (!tx_proc) {
        std::ostringstream os;
        os << "OpenProcess(pid=" << pid << ") failed (GLE " << GetLastError()
           << "). The transmitter process may have exited; start a new transfer and copy the "
              "fresh identifier.";
        throw GpuError(os.str(), -1);
    }
    HANDLE local = nullptr;
    if (!DuplicateHandle(tx_proc, (HANDLE)(uintptr_t)handle_value, GetCurrentProcess(), &local, 0,
                         FALSE, DUPLICATE_SAME_ACCESS)) {
        DWORD gle = GetLastError();
        CloseHandle(tx_proc);
        std::ostringstream os;
        os << "DuplicateHandle failed (GLE " << gle
           << "). The identifier is invalid, stale (transmitter restarted), or already released.";
        throw GpuError(os.str(), -1);
    }
    CloseHandle(tx_proc);

    ComPtr<ID3D12Resource> res;
    HRESULT hr = gpu->device->OpenSharedHandle(local, IID_PPV_ARGS(&res));
    CloseHandle(local);
    if (FAILED(hr))
        fail_hr(hr, "ID3D12Device::OpenSharedHandle",
                "the duplicated handle could not be opened as a shared resource; verify the "
                "identifier and that both processes use the same NVIDIA adapter");

    impl_ = std::make_unique<Impl>();
    impl_->gpu = gpu;
    impl_->resource = std::move(res);
    impl_->size = impl_->resource->GetDesc().Width; // trustworthy driver-reported size
    size_ = impl_->size;

    const uint64_t rb = std::min<uint64_t>(kMaxReadbackChunk, round_up_64(impl_->size, 65536));
    D3D12_HEAP_PROPERTIES rp{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = rb;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    check_hr(gpu->device->CreateCommittedResource(&rp, D3D12_HEAP_FLAG_NONE, &rd,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&impl_->readback)),
             "ID3D12Device::CreateCommittedResource(readback)");
    D3D12_RANGE no_write{0, 0};
    check_hr(impl_->readback->Map(0, &no_write, reinterpret_cast<void**>(&impl_->readback_mapped)),
             "ID3D12Resource::Map(readback)");
    impl_->readback_size = rb;
}

void RxSharedBuffer::read(uint64_t offset, uint8_t* out, size_t len) {
    if (!impl_) throw GpuError("RxSharedBuffer::open was not called", -1);
    if (len == 0) return;
    if (offset > impl_->size || len > impl_->size - offset)
        throw GpuError("read range out of bounds for shared resource", -1);
    if (len > impl_->readback_size)
        throw GpuError("read length exceeds readback chunk size", -1);

    auto* gpu = impl_->gpu;
    auto list = gpu->begin_list();
    list->CopyBufferRegion(impl_->readback.Get(), 0, impl_->resource.Get(), offset, len);
    gpu->submit_and_wait(list.Get());

    D3D12_RANGE valid{0, (SIZE_T)len}; // only [0,len) was written by the GPU
    void* mapped = nullptr;
    check_hr(impl_->readback->Map(0, &valid, &mapped), "ID3D12Resource::Map(readback)");
    memcpy(out, mapped, len);
    D3D12_RANGE none{0, 0};
    impl_->readback->Unmap(0, &none);
}

} // namespace gputxrx::gpu
