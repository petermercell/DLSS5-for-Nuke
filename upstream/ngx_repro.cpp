// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
// ngx_repro - minimal reproduction for NGX DLSS Super Resolution failing at
// EvaluateFeature under Wine + vkd3d-proton + dxvk-nvapi.
//
// Deliberately minimal and *not* a game:
//   * headless console app, no window, no swapchain
//   * only official NVIDIA binaries: the driver's _nvngx.dll and the DLSS SDK's
//     nvngx_dlss.dll. No third-party or redistributed runtime.
//   * feature 1 (DLSS Super Resolution) only
//
// Build on Linux:
//   x86_64-w64-mingw32-g++ -std=c++17 -O2 -o ngx_repro.exe ngx_repro.cpp \\
//       -ld3d12 -ldxgi -static -static-libgcc -static-libstdc++
//
// Run:
//   put _nvngx.dll (from the NVIDIA driver's Wine/NGX directory, e.g.
//   /usr/lib64/nvidia/wine/) and nvngx_dlss.dll (from the public DLSS SDK)
//   next to ngx_repro.exe, then
//
//   WINEDLLOVERRIDES='nvapi,nvapi64=n,b;d3d12,d3d12core=n,b;dxgi,d3d11=n,b' \\
//   wine ./ngx_repro.exe
// -----------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <cwchar>
#include <algorithm>

// ---- Minimal NGX surface (matches the public DLSS SDK ABI) ------------------
typedef uint32_t NGXResult;
#define NGX_OK ((NGXResult)1)

struct NGXHandle { unsigned int Id; };
struct ID3D11Resource;

// IMPORTANT -- vtable ordering.
//
// MSVC places all overloads of one name at the position of the FIRST
// declaration and emits them into the vtable in REVERSE declaration order.
// GCC (and therefore mingw-w64) emits them in declaration order. _nvngx.dll is
// built with MSVC, so transcribing NVIDIA's header literally into a mingw build
// gives a mirror-image vtable: Set(ID3D12Resource*) ends up calling Set(float),
// which reads its argument from XMM2 and never sees the pointer at all. NGX
// then reports "could not find Color parameter" at EvaluateFeature.
//
// Each overload group below is therefore declared reversed. Slot assignments
// verified empirically against the 610.57.04 _nvngx.dll: setting a known
// pointer through slot 0/1 reads back exactly through slot 8/9, while slots
// 3/4/7 store it as a 32/32/64-bit number and 5/6 store register garbage.
struct NGXParameter {
    virtual void Set(const char*, void*) = 0;                            // 0
    virtual void Set(const char*, ID3D12Resource*) = 0;                  // 1
    virtual void Set(const char*, ID3D11Resource*) = 0;                  // 2
    virtual void Set(const char*, int) = 0;                              // 3
    virtual void Set(const char*, unsigned int) = 0;                     // 4
    virtual void Set(const char*, double) = 0;                           // 5
    virtual void Set(const char*, float) = 0;                            // 6
    virtual void Set(const char*, unsigned long long) = 0;               // 7
    virtual NGXResult Get(const char*, void**) const = 0;                // 8
    virtual NGXResult Get(const char*, ID3D12Resource**) const = 0;      // 9
    virtual NGXResult Get(const char*, ID3D11Resource**) const = 0;      // 10
    virtual NGXResult Get(const char*, int*) const = 0;                  // 11
    virtual NGXResult Get(const char*, unsigned int*) const = 0;         // 12
    virtual NGXResult Get(const char*, double*) const = 0;               // 13
    virtual NGXResult Get(const char*, float*) const = 0;                // 14
    virtual NGXResult Get(const char*, unsigned long long*) const = 0;   // 15
    virtual void Reset() = 0;                                            // 16
};

using InitExtFn      = NGXResult(__cdecl*)(unsigned long long, const wchar_t*, ID3D12Device*, int, const void*);
using AllocParamsFn  = NGXResult(__cdecl*)(NGXParameter**);
using CapParamsFn    = NGXResult(__cdecl*)(NGXParameter**);
using CreateFeatureFn= NGXResult(__cdecl*)(ID3D12GraphicsCommandList*, int, NGXParameter*, NGXHandle**);
using EvalFeatureFn  = NGXResult(__cdecl*)(ID3D12GraphicsCommandList*, const NGXHandle*, const NGXParameter*, void*);
using ShutdownFn     = NGXResult(__cdecl*)();

static const unsigned long long APP_ID = 0x4E475850ULL;   // arbitrary
static const int   DLSS_FEATURE_ID     = 1;               // NVSDK_NGX_Feature_SuperSampling
static const UINT  IN_W = 512, IN_H = 288;
static const UINT  OUT_W = 1024, OUT_H = 576;

#define CHECK(expr, msg) do { if (FAILED(expr)) { std::printf("FAIL: %s\n", msg); return 1; } } while (0)

static void PrintResult(const char* what, NGXResult r) {
    std::printf("%-46s -> 0x%08X%s\n", what, (unsigned)r, r == NGX_OK ? "  (OK)" : "");
}

// ---- Direct vtable probing --------------------------------------------------
// Two candidate C++ layouts for NVSDK_NGX_Parameter (with and without the
// ID3D11Resource overloads) have both been eliminated: Set(ID3D12Resource*)
// through either one does not round-trip, and NGX reports
// "could not find Color parameter" at Evaluate. Rather than guess a third
// layout, call the slots by index and find empirically which Set slot the
// resource has to go through.
//
// On the MS x64 ABI a non-overloaded free function with a leading void* takes
// the same registers as a member function, so a vtable slot can be invoked
// directly through these signatures.
using RawSetFn = void      (*)(void* self, const char* name, void* value);
using RawGetFn = NGXResult (*)(void* self, const char* name, void* out);

static bool IsCodePtr(const void* p) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ |
                       PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & exec) != 0;
}

static void DescribePtr(const void* p, char* out, size_t n) {
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)p, &mod) && mod) {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(mod, path, MAX_PATH);
        const char* base = std::strrchr(path, '\\');
        base = base ? base + 1 : path;
        std::snprintf(out, n, "%s+0x%llX", base,
                      (unsigned long long)((uintptr_t)p - (uintptr_t)mod));
    } else {
        std::snprintf(out, n, "(not in a loaded module)");
    }
}

static ID3D12Resource* MakeTex(ID3D12Device* dev, UINT w, UINT h, DXGI_FORMAT fmt,
                               D3D12_RESOURCE_STATES state) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = fmt; d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    ID3D12Resource* r = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr,
                                            IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("ngx_repro: DLSS Super Resolution (NGX feature 1), headless D3D12\n\n");

    // ---- 1. D3D12 device on the NVIDIA adapter ------------------------------
    IDXGIFactory4* factory = nullptr;
    CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");

    ID3D12Device* device = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 ad{};
        adapter->GetDesc1(&ad);
        std::wprintf(L"adapter %u: %ls\n", i, ad.Description);
        std::printf("           vendor=0x%04X device=0x%04X flags=0x%X\n",
                    ad.VendorId, ad.DeviceId, ad.Flags);

        if (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { std::printf("           skipped (software)\n"); adapter->Release(); continue; }

        if (ad.VendorId != 0x10DE) {
            // DXVK reports NVIDIA GPUs with AMD's vendor id unless
            // DXVK_ENABLE_NVAPI=1. Recognise that rather than silently skipping
            // the only usable adapter.
            if (wcsstr(ad.Description, L"NVIDIA")) {
                std::printf("           description says NVIDIA but vendor id is 0x%04X:\n"
                            "           DXVK is hiding the GPU. Set DXVK_ENABLE_NVAPI=1.\n", ad.VendorId);
                std::printf("           trying it anyway\n");
            } else {
                std::printf("           skipped (not NVIDIA)\n");
                adapter->Release();
                continue;
            }
        }

        HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
        std::printf("           D3D12CreateDevice(FL 12_0) -> 0x%08X%s\n",
                    (unsigned)hr, SUCCEEDED(hr) ? "  (OK)" : "");
        adapter->Release();
        if (SUCCEEDED(hr)) break;
        device = nullptr;
    }
    if (!device) {
        std::printf("\nFAIL: no D3D12 device on an NVIDIA adapter.\n");
        std::printf("If DXVK listed the GPU above but D3D12CreateDevice failed, d3d12.dll is\n");
        std::printf("almost certainly Wine's builtin rather than vkd3d-proton. Check that the\n");
        std::printf("run printed vkd3d-proton lines, and that the prefix has vkd3d-proton's\n");
        std::printf("d3d12.dll / d3d12core.dll with WINEDLLOVERRIDES including d3d12,d3d12core=n.\n");
        return 1;
    }

    ID3D12CommandQueue* queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    CHECK(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "CreateCommandQueue");

    ID3D12CommandAllocator* alloc = nullptr;
    CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)),
          "CreateCommandAllocator");

    ID3D12GraphicsCommandList* cmd = nullptr;
    CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
                                    IID_PPV_ARGS(&cmd)), "CreateCommandList");
    std::printf("d3d12 device + command list: OK\n\n");

    // ---- 2. Load the driver's NGX core --------------------------------------
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir(exePath);
    dir = dir.substr(0, dir.find_last_of(L"\\/"));

    HMODULE core = LoadLibraryW((dir + L"\\_nvngx.dll").c_str());
    if (!core) core = LoadLibraryW(L"_nvngx.dll");
    if (!core) { std::printf("FAIL: could not load _nvngx.dll (put it next to this exe)\n"); return 1; }
    std::printf("_nvngx.dll loaded\n");

    if (GetFileAttributesW((dir + L"\\nvngx_dlss.dll").c_str()) == INVALID_FILE_ATTRIBUTES)
        std::printf("NOTE: nvngx_dlss.dll not found next to this exe; NGX will not find the snippet\n");

    auto Init      = (InitExtFn)      GetProcAddress(core, "NVSDK_NGX_D3D12_Init_Ext");
    auto AllocPar  = (AllocParamsFn)  GetProcAddress(core, "NVSDK_NGX_D3D12_AllocateParameters");
    auto CapPar    = (CapParamsFn)    GetProcAddress(core, "NVSDK_NGX_D3D12_GetCapabilityParameters");
    auto CreateFt  = (CreateFeatureFn)GetProcAddress(core, "NVSDK_NGX_D3D12_CreateFeature");
    auto EvalFt    = (EvalFeatureFn)  GetProcAddress(core, "NVSDK_NGX_D3D12_EvaluateFeature");
    auto Shutdown  = (ShutdownFn)     GetProcAddress(core, "NVSDK_NGX_D3D12_Shutdown");
    if (!Init || !AllocPar || !CreateFt || !EvalFt) {
        std::printf("FAIL: missing NGX entry points in _nvngx.dll\n"); return 1;
    }

    // ---- 3. NGX init --------------------------------------------------------
    NGXResult r = 0;
    bool inited = false;
    for (int ver = 0x13; ver <= 0x20 && !inited; ++ver) {
        r = Init(APP_ID, dir.c_str(), device, ver, nullptr);
        if (r == NGX_OK) { std::printf("NVSDK_NGX_D3D12_Init_Ext(ver=0x%02X)          -> 0x%08X  (OK)\n", ver, r); inited = true; }
    }
    if (!inited) { PrintResult("NVSDK_NGX_D3D12_Init_Ext (all versions)", r); return 1; }

    // ---- 4. Capability query ------------------------------------------------
    if (CapPar) {
        NGXParameter* caps = nullptr;
        if (CapPar(&caps) == NGX_OK && caps) {
            int avail = 0; unsigned int needsUpdate = 0;
            caps->Get("SuperSampling.Available", &avail);
            caps->Get("SuperSampling.NeedsUpdatedDriver", &needsUpdate);
            std::printf("SuperSampling.Available                        -> %d\n", avail);
            std::printf("SuperSampling.NeedsUpdatedDriver               -> %u\n", needsUpdate);
        }
    }

    // ---- 5. Create DLSS SR --------------------------------------------------
    NGXParameter* params = nullptr;
    r = AllocPar(&params);
    PrintResult("NVSDK_NGX_D3D12_AllocateParameters", r);
    if (r != NGX_OK || !params) return 1;

    params->Set("Width",            (unsigned int)IN_W);
    params->Set("Height",           (unsigned int)IN_H);
    params->Set("OutWidth",         (unsigned int)OUT_W);
    params->Set("OutHeight",        (unsigned int)OUT_H);
    params->Set("PerfQualityValue", 2);              // MaxQuality
    params->Set("DLSS.Feature.Create.Flags",
                (1 << 0) | (1 << 6));                // IsHDR | AutoExposure

    NGXHandle* feature = nullptr;
    r = CreateFt(cmd, DLSS_FEATURE_ID, params, &feature);
    PrintResult("NVSDK_NGX_D3D12_CreateFeature(1: DLSS SR)", r);
    if (r != NGX_OK || !feature) return 1;

    // ---- 6. Resources -------------------------------------------------------
    ID3D12Resource* color  = MakeTex(device, IN_W,  IN_H,  DXGI_FORMAT_R16G16B16A16_FLOAT,
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* output = MakeTex(device, OUT_W, OUT_H, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* mvec   = MakeTex(device, IN_W,  IN_H,  DXGI_FORMAT_R16G16_FLOAT,
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* depth  = MakeTex(device, IN_W,  IN_H,  DXGI_FORMAT_R32_FLOAT,
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (!color || !output || !mvec || !depth) {
        std::printf("FAIL: resource creation\n"); return 1;
    }
    std::printf("resources created (RGBA16F / RG16F / R32F, all ALLOW_UNORDERED_ACCESS)\n\n");

    // ---- 6b. Enumerate the parameter object's vtable -------------------------
    // The numeric setters demonstrably work (CreateFeature read Width/Height
    // and PerfQualityValue correctly), so slots 0..4 match the public header.
    // The resource setters do not round-trip through either candidate layout,
    // so find the right slot by scanning instead of by guessing.
    void** vt = *reinterpret_cast<void***>(params);
    int nslots = 0;
    {
        std::printf("--- vtable of the object from AllocateParameters (%p) ---\n", (void*)vt);
        for (int i = 0; i < 24; ++i) {
            void* fn = vt[i];
            if (!IsCodePtr(fn)) {
                std::printf("slot %2d: %p  not code -- vtable ends here\n", i, fn);
                break;
            }
            char desc[MAX_PATH + 32];
            DescribePtr(fn, desc, sizeof desc);
            // Slots that share a target are the same function reached through
            // different overloads (common for the integer setters).
            int sameAs = -1;
            for (int j = 0; j < i; ++j) if (vt[j] == fn) { sameAs = j; break; }
            std::printf("slot %2d: %p  %s%s\n", i, fn, desc,
                        sameAs >= 0 ? "   (same target as an earlier slot)" : "");
            nslots = i + 1;
        }
        std::printf("--- %d slots ---\n\n", nslots);
    }

    // ---- 6c. Confirm the reversed layout lines up with the real vtable ------
    // Compare the compiler's idea of each overload's slot against the addresses
    // dumped above. Any mismatch here means this build's overload ordering has
    // drifted from _nvngx.dll again.
    {
        std::printf("--- overload slots as this compiler emitted them ---\n");
        struct { const char* what; int expect; } chk[] = {
            {"Set(void*)",            0}, {"Set(ID3D12Resource*)",  1},
            {"Set(ID3D11Resource*)",  2}, {"Set(int)",              3},
            {"Set(unsigned int)",     4}, {"Set(double)",           5},
            {"Set(float)",            6}, {"Set(unsigned long long)", 7},
        };
        for (auto& c : chk)
            std::printf("  %-26s expected slot %d -> %p\n", c.what, c.expect,
                        c.expect < nslots ? vt[c.expect] : nullptr);
        std::printf("--- end ---\n\n");
    }

    // ---- 6d. Does the resource survive Set -> Get now? ----------------------
    {
        ID3D12Resource* back = nullptr;
        params->Set("Color", color);
        NGXResult g = params->Get("Color", &back);
        std::printf("Set(\"Color\", ID3D12Resource*) then Get -> 0x%08X, %p (expected %p)  %s\n\n",
                    (unsigned)g, (void*)back, (void*)color,
                    (g == NGX_OK && back == color)
                        ? "MATCH -- the parameter now reaches NGX"
                        : "MISMATCH -- still wrong");
    }

    // ---- 6e. Put a real image in the input ---------------------------------
    // Without this the colour texture is undefined, and a black output would
    // prove nothing. Upload a gradient with a bright band so a correct result
    // is unmistakable.
    {
        D3D12_RESOURCE_DESC cd = color->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT rows = 0; UINT64 rowBytes = 0, total = 0;
        device->GetCopyableFootprints(&cd, 0, 1, 0, &fp, &rows, &rowBytes, &total);

        D3D12_HEAP_PROPERTIES up{}; up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC ub{};
        ub.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        ub.Width = total; ub.Height = 1; ub.DepthOrArraySize = 1; ub.MipLevels = 1;
        ub.Format = DXGI_FORMAT_UNKNOWN; ub.SampleDesc.Count = 1;
        ub.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ID3D12Resource* upload = nullptr;
        if (SUCCEEDED(device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &ub,
                                                      D3D12_RESOURCE_STATE_GENERIC_READ,
                                                      nullptr, IID_PPV_ARGS(&upload)))) {
            void* p = nullptr;
            D3D12_RANGE none{ 0, 0 };
            if (SUCCEEDED(upload->Map(0, &none, &p)) && p) {
                for (UINT y = 0; y < IN_H; ++y) {
                    uint16_t* row = (uint16_t*)((uint8_t*)p + fp.Footprint.RowPitch * y);
                    for (UINT x = 0; x < IN_W; ++x) {
                        // half-float literals: 0x3C00 = 1.0, 0x3800 = 0.5, 0x4400 = 4.0
                        const bool band = (y > IN_H / 2 && y < IN_H / 2 + 8);
                        row[x * 4 + 0] = band ? 0x4400 : (uint16_t)(0x3800);
                        row[x * 4 + 1] = (uint16_t)((x & 31) ? 0x3800 : 0x3C00);
                        row[x * 4 + 2] = (uint16_t)((y & 31) ? 0x3400 : 0x3C00);
                        row[x * 4 + 3] = 0x3C00;
                    }
                }
                upload->Unmap(0, nullptr);

                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource   = color;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
                cmd->ResourceBarrier(1, &b);

                D3D12_TEXTURE_COPY_LOCATION s{}, d{};
                s.pResource = upload;
                s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                s.PlacedFootprint = fp;
                d.pResource = color;
                d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                d.SubresourceIndex = 0;
                cmd->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);

                std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
                cmd->ResourceBarrier(1, &b);
                std::printf("input image uploaded (%ux%u gradient with a bright band)\n\n",
                            IN_W, IN_H);
            }
        }
    }

    // ---- 7. Evaluate --------------------------------------------------------
    params->Set("Color",         color);
    params->Set("Output",        output);
    params->Set("MotionVectors", mvec);
    params->Set("Depth",         depth);
    params->Set("Reset",           1);
    params->Set("Jitter.Offset.X", 0.0f);
    params->Set("Jitter.Offset.Y", 0.0f);
    params->Set("MV.Scale.X",      1.0f);
    params->Set("MV.Scale.Y",      1.0f);
    params->Set("DLSS.Render.Subrect.Dimensions.Width",  (unsigned int)IN_W);
    params->Set("DLSS.Render.Subrect.Dimensions.Height", (unsigned int)IN_H);

    r = EvalFt(cmd, feature, params, nullptr);
    PrintResult("NVSDK_NGX_D3D12_EvaluateFeature", r);

    // ---- 8. Actually run it, and look at the pixels -------------------------
    // EvaluateFeature only RECORDS work into the command list. A successful
    // return says the call was accepted, not that anything was computed, so
    // close/execute/fence and read the output back before believing it.
    if (r == NGX_OK) {
        std::printf("\n--- executing the command list and reading the output back ---\n");

        D3D12_RESOURCE_BARRIER toCopy{};
        toCopy.Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopy.Transition.pResource   = output;
        toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toCopy.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cmd->ResourceBarrier(1, &toCopy);

        D3D12_RESOURCE_DESC od = output->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT   rows = 0;
        UINT64 rowBytes = 0, total = 0;
        device->GetCopyableFootprints(&od, 0, 1, 0, &fp, &rows, &rowBytes, &total);

        D3D12_HEAP_PROPERTIES rbHeap{}; rbHeap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC   rbDesc{};
        rbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rbDesc.Width = total; rbDesc.Height = 1; rbDesc.DepthOrArraySize = 1;
        rbDesc.MipLevels = 1; rbDesc.Format = DXGI_FORMAT_UNKNOWN;
        rbDesc.SampleDesc.Count = 1;
        rbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ID3D12Resource* readback = nullptr;
        if (FAILED(device->CreateCommittedResource(&rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&readback)))) {
            std::printf("  FAIL: readback buffer\n");
        } else {
            D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
            src.pResource = output;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = 0;
            dst.pResource = readback;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = fp;
            cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

            HRESULT hrClose = cmd->Close();
            std::printf("  Close()  -> 0x%08X\n", (unsigned)hrClose);

            ID3D12CommandList* lists[] = { cmd };
            queue->ExecuteCommandLists(1, lists);

            ID3D12Fence* fence = nullptr;
            device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
            HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            queue->Signal(fence, 1);
            if (fence->GetCompletedValue() < 1) {
                fence->SetEventOnCompletion(1, ev);
                DWORD w = WaitForSingleObject(ev, 15000);
                std::printf("  GPU wait -> %s\n", w == WAIT_OBJECT_0 ? "signalled" : "TIMED OUT");
            }
            std::printf("  device removed? 0x%08X\n", (unsigned)device->GetDeviceRemovedReason());

            void* mapped = nullptr;
            D3D12_RANGE readAll{ 0, (SIZE_T)total };
            if (SUCCEEDED(readback->Map(0, &readAll, &mapped)) && mapped) {
                const uint16_t* h = (const uint16_t*)mapped;
                size_t n = (size_t)(total / 2);
                size_t nonzero = 0;
                uint16_t first = h[0];
                bool allSame = true;
                for (size_t i = 0; i < n; ++i) {
                    if (h[i] != 0)     ++nonzero;
                    if (h[i] != first) allSame = false;
                }
                std::printf("  output %llu bytes, %zu of %zu halfs non-zero, %s\n",
                            (unsigned long long)total, nonzero, n,
                            allSame ? "ALL IDENTICAL" : "varying");
                std::printf("  first 8 halfs: ");
                for (int i = 0; i < 8 && (size_t)i < n; ++i) std::printf("%04X ", h[i]);
                std::printf("\n");
                if (nonzero == 0)
                    std::printf("  >>> Evaluate reported success but wrote NOTHING.\n");
                else
                    std::printf("  >>> real pixels came out.\n");
                D3D12_RANGE noWrite{ 0, 0 };
                readback->Unmap(0, &noWrite);
            } else {
                std::printf("  FAIL: could not map the readback buffer\n");
            }
        }
        std::printf("--- end ---\n");
    }

    std::printf("\n");
    if (r == NGX_OK) {
        std::printf("RESULT: PASS - DLSS SR evaluated.\n");
    } else if (r == 0xBAD00005u) {
        std::printf("RESULT: FAIL - 0xBAD00005 NVSDK_NGX_Result_FAIL_InvalidParameter.\n");
        std::printf("        Init and CreateFeature succeeded; only Evaluate is refused.\n");
        std::printf("        If the Set/Get of \"Color\" above says MATCH, the vtable\n");
        std::printf("        ordering is right and something else is missing; check the\n");
        std::printf("        NGX log for which parameter it names this time.\n");
    } else {
        std::printf("RESULT: FAIL - 0x%08X\n", (unsigned)r);
    }

    if (Shutdown) Shutdown();
    return r == NGX_OK ? 0 : 2;
}
