// QuarryDLSS probe: a ReShade add-on that maps The Quarry's renderer before any
// DLSS work. It logs which graphics API and swap chain the game uses, finds the
// temporal anti-aliasing pass and records what that pass reads and writes.
//
// How the TAA pass is found: a temporal pass reads last frame's output of the
// same shader (its history). Every post-process dispatch and full-screen draw is
// tracked; a shader that reads a texture it wrote itself one or two frames ago
// is a temporal pass. Unreal also uses TemporalAA.usf for SSR, depth of field
// and light shafts, so the pass with the largest output is reported as the main
// TAA guess.
//
// Only the t#/u#/cb# slots a shader declares in its DXBC are listed, because the
// D3D11 RHI leaves earlier views bound. Constant buffers are mirrored on the CPU
// (creation data, Map/Unmap, UpdateSubresource) so a pass's constants can be
// dumped, which is where the TAA jitter lives.
//
// Output, next to the game executable:
//   QuarryDLSS.log                 API, swap chain, every temporal pass found
//   QuarryDLSS_frame_<n>.txt       full pass list of one frame (Ctrl+F9 or the overlay)
//   QuarryDLSS_cbuffers_<n>.txt    constants of the large temporal passes, 8 frames (Ctrl+F10)
//
// Direct3D 12 (-dx12) is covered by mirroring the descriptor heaps and root
// signatures (see "Direct3D 12 descriptors") and reading constant buffers from the
// game's persistently mapped upload memory.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cvars.hpp"
#include "dlss.hpp"
#include "dxbc.hpp"
#include "fg.hpp"
#include "version.h"

#pragma comment(lib, "dxgi.lib")

extern "C" __declspec(dllexport) const char* NAME = "QuarryDLSS";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "NVIDIA DLSS Super Resolution for The Quarry (DirectX 12), in place of the game's temporal upscaler.";

namespace {

using namespace reshade::api;

constexpr uint32_t kMaxSrv = 32;
constexpr uint32_t kMaxUav = 8;
constexpr uint32_t kMaxRtv = 8;
constexpr uint32_t kMaxCb = 14;
constexpr uint64_t kGeometryKey = 0;  // writer key for scene geometry draws

// ---------------------------------------------------------------- logging

// Guards everything below; events arrive on the RHI thread. Recursive because the
// probe maps D3D12 buffers itself, which can re-enter its own map event.
std::recursive_mutex g_mutex;
FILE* g_log = nullptr;
std::wstring g_dir;  // game executable folder, with trailing slash

void Log(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_log) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(g_log, "%02u:%02u:%02u.%03u %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buf);
        fflush(g_log);
    }
}

void OpenLog() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    g_dir.assign(path, n);
    g_dir.resize(g_dir.find_last_of(L"\\/") + 1);
    std::wstring log = g_dir + L"QuarryDLSS.log";
    std::wstring prev = g_dir + L"QuarryDLSS.prev.log";
    // ReShade loads and unloads the add-on for each device the game creates at
    // startup, so only the first load in a process starts a new log.
    if (GetEnvironmentVariableW(L"QDLSS_LOG_OPEN", nullptr, 0) == 0) {
        SetEnvironmentVariableW(L"QDLSS_LOG_OPEN", L"1");
        MoveFileExW(log.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    g_log = _wfopen(log.c_str(), L"a");
}

std::string Narrow(const wchar_t* s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
}

// Executable identity, for the per-build table the real mod will need.
void LogGameInfo() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    auto base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    Log("QuarryDLSS %s", QDLSS_VERSION);
    Log("exe: %s", Narrow(path).c_str());
    Log("exe: PE timestamp 0x%08lx, image size 0x%lx", static_cast<unsigned long>(nt->FileHeader.TimeDateStamp),
        static_cast<unsigned long>(nt->OptionalHeader.SizeOfImage));
    Log("command line: %s", Narrow(GetCommandLineW()).c_str());
}

const char* FormatName(format f) {
    switch (static_cast<uint32_t>(f)) {
        case 0: return "unknown";
        case 2: return "RGBA32F";
        case 9: return "RGBA16_TYPELESS";
        case 10: return "RGBA16F";
        case 11: return "RGBA16_UNORM";
        case 15: return "RG32_TYPELESS";
        case 16: return "RG32F";
        case 19: return "R32G8X24_TYPELESS(depth)";
        case 20: return "D32F_S8";
        case 21: return "R32F_X8(depth)";
        case 22: return "X32_G8(stencil)";
        case 23: return "RGB10A2_TYPELESS";
        case 24: return "RGB10A2_UNORM";
        case 26: return "R11G11B10F";
        case 27: return "RGBA8_TYPELESS";
        case 28: return "RGBA8_UNORM";
        case 29: return "RGBA8_SRGB";
        case 33: return "RG16_TYPELESS";
        case 34: return "RG16F";
        case 35: return "RG16_UNORM";
        case 39: return "R32_TYPELESS";
        case 40: return "D32F";
        case 41: return "R32F";
        case 42: return "R32_UINT";
        case 44: return "R24G8_TYPELESS(depth)";
        case 45: return "D24_S8";
        case 46: return "R24_X8(depth)";
        case 47: return "X24_G8(stencil)";
        case 48: return "RG8_TYPELESS";
        case 49: return "RG8_UNORM";
        case 53: return "R16_TYPELESS";
        case 54: return "R16F";
        case 55: return "D16";
        case 56: return "R16_UNORM";
        case 57: return "R16_UINT";
        case 60: return "R8_TYPELESS";
        case 61: return "R8_UNORM";
        case 62: return "R8_UINT";
        case 87: return "BGRA8_UNORM";
        case 90: return "BGRA8_TYPELESS";
        case 91: return "BGRA8_SRGB";
        default: {
            static thread_local char buf[16];
            snprintf(buf, sizeof(buf), "fmt%u", static_cast<uint32_t>(f));
            return buf;
        }
    }
}

const char* ApiName(device_api api) {
    switch (api) {
        case device_api::d3d9: return "Direct3D 9";
        case device_api::d3d10: return "Direct3D 10";
        case device_api::d3d11: return "Direct3D 11";
        case device_api::d3d12: return "Direct3D 12";
        case device_api::opengl: return "OpenGL";
        case device_api::vulkan: return "Vulkan";
    }
    return "unknown";
}

// ---------------------------------------------------------------- shaders

// Shader identity: the first 8 bytes of the DXBC container digest (stable across
// runs and between D3D11 and the shader cache), or FNV-1a for anything else.
uint64_t ShaderKey(const shader_desc* sd) {
    if (!sd || !sd->code || !sd->code_size) return 0;
    auto p = static_cast<const uint8_t*>(sd->code);
    if (sd->code_size >= 20 && memcmp(p, "DXBC", 4) == 0) {
        uint64_t k;
        memcpy(&k, p + 4, 8);
        return k ? k : 1;
    }
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < sd->code_size; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h ? h : 1;
}

struct PipelineInfo {
    uint64_t vs = 0, ps = 0, cs = 0;
};
std::unordered_map<uint64_t, PipelineInfo> g_pipelines;
std::unordered_map<uint64_t, ShaderDecl> g_decls;  // by shader key

const ShaderDecl* FindDecl(uint64_t key) {
    auto it = g_decls.find(key);
    return it != g_decls.end() && it->second.valid ? &it->second : nullptr;
}

// ---------------------------------------------------------------- Direct3D 12 descriptors
//
// D3D12 binds through descriptor heaps, so the probe mirrors them: every view the
// game creates (update_descriptor_tables) and every CopyDescriptors into the
// shader-visible heap (copy_descriptor_tables) is recorded per heap offset. Root
// signatures (init_pipeline_layout) map a shader register to a root parameter and
// an offset in the table bound there (bind_descriptor_tables).

constexpr uint32_t kMaxRootParams = 64;

struct HeapEntry {
    uint64_t res = 0;       // resource (buffer for a CBV)
    uint64_t cbOffset = 0;  // CBV: byte offset in the buffer
    uint32_t viewFormat = 0;
    uint8_t kind = 0;       // 0 empty, 1 SRV, 2 UAV, 3 CBV
};
std::unordered_map<uint64_t, std::vector<HeapEntry>> g_heaps;  // descriptor heap -> entries by offset

struct LayoutParam {
    pipeline_layout_param_type type;
    std::vector<descriptor_range> ranges;  // table ranges with resolved offsets, or the one root descriptor
};
std::unordered_map<uint64_t, std::vector<LayoutParam>> g_layouts;  // root signature -> parameters

std::unordered_set<uint64_t> g_live;  // D3D12 resources not yet destroyed
uint64_t g_descriptorWrites = 0, g_descriptorCopies = 0;

enum class Slot { srv, uav, cbv };

bool RangeIs(descriptor_type t, Slot s) {
    switch (s) {
        case Slot::srv:
            return t == descriptor_type::shader_resource_view || t == descriptor_type::buffer_shader_resource_view ||
                   t == descriptor_type::sampler_with_resource_view;
        case Slot::uav:
            return t == descriptor_type::unordered_access_view || t == descriptor_type::buffer_unordered_access_view;
        case Slot::cbv:
            return t == descriptor_type::constant_buffer;
    }
    return false;
}

bool Visible(shader_stage visibility, bool compute) {
    const auto stage = compute ? shader_stage::compute : shader_stage::pixel;
    return (static_cast<uint32_t>(visibility) & static_cast<uint32_t>(stage)) != 0;
}

HeapEntry* HeapSlot(device* dev, descriptor_table table, uint32_t binding, bool create, uint32_t extra = 0) {
    if (!table.handle) return nullptr;
    descriptor_heap heap = {};
    uint32_t offset = 0;
    dev->get_descriptor_heap_offset(table, binding, 0, &heap, &offset);
    if (!heap.handle) return nullptr;
    auto& v = g_heaps[heap.handle];
    if (offset + extra >= v.size()) {
        if (!create) return offset < v.size() ? &v[offset] : nullptr;
        v.resize((size_t(offset) + extra + 4096) & ~size_t(4095));
    }
    return &v[offset];
}

// ---------------------------------------------------------------- per-context state

struct ContextState {
    uint64_t vs = 0, ps = 0, cs = 0;
    resource_view psSrv[kMaxSrv] = {};
    resource_view csSrv[kMaxSrv] = {};
    resource_view csUav[kMaxUav] = {};
    buffer_range psCb[kMaxCb] = {};
    buffer_range csCb[kMaxCb] = {};
    resource_view rtv[kMaxRtv] = {};
    uint32_t rtvCount = 0;
    resource_view dsv = {};
    bool targetsDirty = true;
    viewport vp = {};
    // D3D12: root signature and bound descriptor tables, [0] graphics, [1] compute.
    pipeline_layout layout[2] = {};
    descriptor_table tables[2][kMaxRootParams] = {};
    // D3D12 compute state, so it can be re-bound after DLSS (by root parameter).
    pipeline csPipeline = {};
    buffer_range csRootCbv[kMaxRootParams] = {};
    uint64_t csRootCbvMask = 0;
    std::unordered_map<uint32_t, std::vector<uint32_t>> csRootConstants;
};
std::unordered_map<command_list*, ContextState> g_contexts;

// Who last wrote each resource: shader key (0 = geometry), frame and pass number.
struct Writer {
    uint64_t key;
    uint64_t frame;
    uint32_t pass;
};
std::unordered_map<uint64_t, Writer> g_writers;

// ---------------------------------------------------------------- frame state

uint64_t g_frame = 0;
uint32_t g_pass = 0;           // post-process passes so far this frame
uint32_t g_geometryRun = 0;    // geometry draws since the last logged pass
uint64_t g_dumpFrame = 0;      // frame to write out in full (0 = none)
std::vector<std::string> g_dump;
std::string g_lastDumpFile;

device_api g_api = device_api::d3d11;
bool g_haveDevice = false;
std::string g_adapter;
std::string g_swapchain;  // description of the current back buffer

// A shader found reading its own previous output.
struct Candidate {
    uint64_t key = 0;
    bool compute = false;
    uint64_t hits = 0;
    uint64_t firstFrame = 0, lastFrame = 0;
    uint32_t outW = 0, outH = 0;    // largest output
    uint32_t inW = 0, inH = 0;      // largest input that is not history
    std::string io;                 // latest inputs/outputs, one per line
};
std::unordered_map<uint64_t, Candidate> g_candidates;
uint64_t g_mainKey = 0;
uint32_t g_bbW = 0, g_bbH = 0;  // back buffer size

// CPU copies of every constant buffer (64 KB or less), kept current from the
// game's creation data, Map/Unmap and UpdateSubresource, so a pass's constants
// can be dumped without reading back from the GPU.
std::unordered_map<uint64_t, uint64_t> g_cbSize;               // constant buffer -> size
std::unordered_map<uint64_t, std::vector<uint8_t>> g_cbData;   // constant buffer -> contents
struct PendingMap {
    uint8_t* ptr;
    uint64_t offset, size;
};
std::unordered_map<uint64_t, PendingMap> g_maps;

// Constant buffer dump (Ctrl+F10): every large temporal pass for kCbDumpFrames frames.
constexpr uint64_t kCbDumpFrames = 8;
uint64_t g_cbDumpFirst = 0, g_cbDumpLast = 0;
std::string g_cbDump;
std::string g_lastCbDumpFile;
uint32_t g_mainInW = 0, g_mainInH = 0, g_mainOutW = 0, g_mainOutH = 0;

std::string RenderScaleStatus();  // forced render scale, defined with ApplyRenderScale

// ---------------------------------------------------------------- pass analysis

struct Binding {
    char slot[8];
    resource res;
    resource_desc desc;
    format viewFormat;
    const Writer* writer;  // null when nothing tracked wrote it
};

bool ResolveView(device* dev, resource_view view, Binding& b) {
    if (!view.handle) return false;
    b.res = dev->get_resource_from_view(view);
    if (!b.res.handle) return false;
    b.desc = dev->get_resource_desc(b.res);
    b.viewFormat = dev->get_resource_view_desc(view).format;
    auto it = g_writers.find(b.res.handle);
    b.writer = it == g_writers.end() ? nullptr : &it->second;
    return true;
}

bool IsLive(uint64_t res) { return g_api != device_api::d3d12 || g_live.count(res) != 0; }

// D3D12: the heap entry behind shader register <reg> of the given kind, through the
// bound root signature and descriptor tables.
const HeapEntry* ResolveTableSlot(device* dev, const ContextState& st, bool compute, Slot kind, uint32_t reg) {
    const int g = compute ? 1 : 0;
    auto lit = g_layouts.find(st.layout[g].handle);
    if (lit == g_layouts.end()) return nullptr;
    const auto& params = lit->second;
    for (uint32_t p = 0; p < params.size() && p < kMaxRootParams; ++p) {
        if (params[p].type == pipeline_layout_param_type::push_descriptors ||
            params[p].type == pipeline_layout_param_type::push_constants)
            continue;
        for (const auto& r : params[p].ranges) {
            if (!RangeIs(r.type, kind) || r.dx_register_space != 0 || !Visible(r.visibility, compute) ||
                reg < r.dx_register_index || reg - r.dx_register_index >= r.count)
                continue;
            const HeapEntry* e = HeapSlot(dev, st.tables[g][p], r.binding + (reg - r.dx_register_index), false);
            return e && e->kind ? e : nullptr;
        }
    }
    return nullptr;
}

bool ResolveEntry(device* dev, const HeapEntry* e, Binding& b) {
    if (!e || !e->res || !IsLive(e->res)) return false;
    b.res = {e->res};
    b.desc = dev->get_resource_desc(b.res);
    b.viewFormat = static_cast<format>(e->viewFormat);
    auto it = g_writers.find(b.res.handle);
    b.writer = it == g_writers.end() ? nullptr : &it->second;
    return true;
}

bool IsHistory(const Binding& b, uint64_t key) {
    return b.writer && key != kGeometryKey && b.writer->key == key && b.writer->frame < g_frame &&
           g_frame - b.writer->frame <= 2;
}

std::string Describe(const Binding& b, uint64_t key, bool input) {
    char buf[256];
    const auto& t = b.desc.texture;
    int n;
    if (b.desc.type == resource_type::buffer)
        n = snprintf(buf, sizeof(buf), "%-4s buffer %llu bytes", b.slot,
                     static_cast<unsigned long long>(b.desc.buffer.size));
    else
        n = snprintf(buf, sizeof(buf), "%-4s %ux%u %s%s%s mips=%u res=%llx", b.slot, t.width, t.height,
                     FormatName(b.viewFormat), b.viewFormat != t.format ? " of " : "",
                     b.viewFormat != t.format ? FormatName(t.format) : "", t.levels,
                     static_cast<unsigned long long>(b.res.handle));
    std::string s(buf, n > 0 ? std::min<size_t>(n, sizeof(buf) - 1) : 0);
    if (input && IsHistory(b, key)) {
        s += "  <- HISTORY (own output, last frame)";
    } else if (input && b.writer && b.writer->frame == g_frame) {
        if (b.writer->key == kGeometryKey)
            snprintf(buf, sizeof(buf), "  <- scene geometry");
        else
            snprintf(buf, sizeof(buf), "  <- pass #%u (%016llx)", b.writer->pass,
                     static_cast<unsigned long long>(b.writer->key));
        s += buf;
    }
    return s;
}

void MarkWritten(resource res, uint64_t key) {
    if (res.handle) g_writers[res.handle] = {key, g_frame, g_pass};
}

std::unordered_map<uint64_t, uint8_t*> g_mapped;  // D3D12 buffer -> CPU address of byte 0 (persistent maps)

// Copies up to <bytes> of a constant buffer: from the shadow copy on D3D11, from the
// game's persistently mapped upload memory on D3D12 (mapping it ourselves if needed).
bool ReadConstants(device* dev, const buffer_range& r, uint64_t bytes, std::vector<uint8_t>& out) {
    out.clear();
    if (!r.buffer.handle) return false;
    if (g_api != device_api::d3d12) {
        auto it = g_cbData.find(r.buffer.handle);
        if (it == g_cbData.end() || r.offset >= it->second.size()) return false;
        bytes = std::min<uint64_t>(bytes, it->second.size() - r.offset);
        out.assign(it->second.begin() + r.offset, it->second.begin() + r.offset + bytes);
        return true;
    }
    if (!IsLive(r.buffer.handle)) return false;
    const auto desc = dev->get_resource_desc(r.buffer);
    if (desc.type != resource_type::buffer || r.offset >= desc.buffer.size) return false;
    bytes = std::min<uint64_t>(bytes, desc.buffer.size - r.offset);
    out.resize(bytes);
    if (auto it = g_mapped.find(r.buffer.handle); it != g_mapped.end()) {
        memcpy(out.data(), it->second + r.offset, bytes);
        return true;
    }
    void* p = nullptr;
    if (desc.heap == memory_heap::default_) return false;  // not CPU-visible
    if (!dev->map_buffer_region(r.buffer, r.offset, bytes, map_access::read_only, &p) || !p) return false;
    memcpy(out.data(), p, bytes);
    dev->unmap_buffer_region(r.buffer);
    return true;
}

// Each declared constant buffer as float4 rows: values, then raw bits.
std::string DumpConstantBuffers(device* dev, const buffer_range* cbs, const ShaderDecl* decl) {
    std::string s;
    char line[192];
    std::vector<uint8_t> data;
    for (uint32_t i = 0; i < kMaxCb; ++i) {
        if (decl ? !decl->HasCb(i) : !cbs[i].buffer.handle) continue;
        const buffer_range& r = cbs[i];
        const uint32_t declared = decl ? decl->cbVec4[i] : 0;
        snprintf(line, sizeof(line), "  cb%u: buffer %llx, offset %llu, declared %u float4\n", i,
                 static_cast<unsigned long long>(r.buffer.handle), static_cast<unsigned long long>(r.offset), declared);
        s += line;
        if (!ReadConstants(dev, r, declared ? declared * 16ull : 4096ull, data)) {
            s += "    (contents not captured)\n";
            continue;
        }
        const uint64_t rows = data.size() / 16;
        for (uint64_t row = 0; row < rows; ++row) {
            float f[4];
            uint32_t u[4];
            memcpy(f, data.data() + row * 16, 16);
            memcpy(u, f, 16);
            snprintf(line, sizeof(line), "    [%3llu] %14.7g %14.7g %14.7g %14.7g | %08x %08x %08x %08x\n",
                     static_cast<unsigned long long>(row), f[0], f[1], f[2], f[3], u[0], u[1], u[2], u[3]);
            s += line;
        }
    }
    return s;
}

// A dispatch or full-screen draw: work out its inputs and outputs, flag history
// reads, and record what it writes.
void OnPass(command_list* cmd, bool compute, const char* what) {
    auto& st = g_contexts[cmd];
    const uint64_t key = compute ? st.cs : st.ps;
    device* dev = cmd->get_device();

    // Only slots the shader declares; the D3D11 RHI leaves earlier views bound.
    const ShaderDecl* decl = FindDecl(key);
    const bool d3d12 = g_api == device_api::d3d12;
    std::vector<Binding> in, out;
    const resource_view* srv = compute ? st.csSrv : st.psSrv;
    // D3D12 needs the declarations to know which registers to look up; up to t127.
    for (uint32_t i = 0; i < (d3d12 ? 128u : kMaxSrv); ++i) {
        if (decl ? !decl->HasSrv(i) : d3d12) continue;
        Binding b{};
        if (d3d12 ? ResolveEntry(dev, ResolveTableSlot(dev, st, compute, Slot::srv, i), b) : ResolveView(dev, srv[i], b)) {
            snprintf(b.slot, sizeof(b.slot), "t%u", i);
            in.push_back(b);
        }
    }
    if (compute) {
        for (uint32_t i = 0; i < (d3d12 ? 64u : kMaxUav); ++i) {
            if (decl ? !decl->HasUav(i) : d3d12) continue;
            Binding b{};
            if (d3d12 ? ResolveEntry(dev, ResolveTableSlot(dev, st, true, Slot::uav, i), b)
                      : ResolveView(dev, st.csUav[i], b)) {
                snprintf(b.slot, sizeof(b.slot), "u%u", i);
                out.push_back(b);
            }
        }
    } else {
        for (uint32_t i = 0; i < st.rtvCount && i < kMaxRtv; ++i) {
            Binding b{};
            if (ResolveView(dev, st.rtv[i], b)) {
                snprintf(b.slot, sizeof(b.slot), "rt%u", i);
                out.push_back(b);
            }
        }
    }

    // Last frame's DLSS output read back by the game's lighting (SSGI/SSR): swap in the
    // clamped copy before this pass runs, so the feedback loop can't build up.
    if (d3d12)
        for (const auto& b : in)
            if (dlss::PatchLightingHistory(cmd, b.res, compute, g_frame)) break;

    bool history = false;
    uint32_t inW = 0, inH = 0, outW = 0, outH = 0;
    for (const auto& b : in) {
        if (IsHistory(b, key)) {
            history = true;
        } else if (b.desc.type != resource_type::buffer &&
                   uint64_t(b.desc.texture.width) * b.desc.texture.height > uint64_t(inW) * inH) {
            inW = b.desc.texture.width;
            inH = b.desc.texture.height;
        }
    }
    for (const auto& b : out)
        if (b.desc.type != resource_type::buffer &&
            uint64_t(b.desc.texture.width) * b.desc.texture.height > uint64_t(outW) * outH) {
            outW = b.desc.texture.width;
            outH = b.desc.texture.height;
        }

    const bool dumping = g_dumpFrame && g_frame == g_dumpFrame;
    if (history || dumping) {
        std::string io;
        char head[160];
        snprintf(head, sizeof(head), "%s %s %016llx%s", what, compute ? "CS" : "PS",
                 static_cast<unsigned long long>(key), history ? "  [TEMPORAL]" : "");
        if (!compute)
            snprintf(head + strlen(head), sizeof(head) - strlen(head), "  viewport %.0fx%.0f+%.0f+%.0f",
                     st.vp.width, st.vp.height, st.vp.x, st.vp.y);
        if (!decl) io += "    (shader declarations not parsed: listing every bound slot)\n";
        for (const auto& b : in) io += "    in  " + Describe(b, key, true) + "\n";
        for (const auto& b : out) io += "    out " + Describe(b, key, false) + "\n";

        if (dumping) {
            char line[64];
            if (g_geometryRun) {
                snprintf(line, sizeof(line), "      (%u geometry draws)", g_geometryRun);
                g_dump.push_back(line);
            }
            snprintf(line, sizeof(line), "#%-4u ", g_pass);
            g_dump.push_back(line + std::string(head) + "\n" + io);
        }
        if (history && key) {
            auto& c = g_candidates[key];
            if (!c.hits) {
                c.key = key;
                c.compute = compute;
                c.firstFrame = g_frame;
                Log("temporal pass found at frame %llu, pass #%u: %s\n%s",
                    static_cast<unsigned long long>(g_frame), g_pass, head, io.c_str());
            }
            ++c.hits;
            c.lastFrame = g_frame;
            c.outW = outW;
            c.outH = outH;
            c.inW = inW;
            c.inH = inH;
            c.io = std::string(head) + "\n" + io;
        }
    }
    g_geometryRun = 0;

    // Constants of the large temporal passes (the main TAA and its look-alikes).
    if (history && g_cbDumpFirst && g_frame >= g_cbDumpFirst && g_frame <= g_cbDumpLast &&
        uint64_t(outW) * outH * 4 >= uint64_t(g_bbW) * g_bbH) {
        char head[192];
        snprintf(head, sizeof(head), "frame %llu pass #%u %s %016llx  in %ux%u -> out %ux%u\n",
                 static_cast<unsigned long long>(g_frame), g_pass, compute ? "CS" : "PS",
                 static_cast<unsigned long long>(key), inW, inH, outW, outH);
        g_cbDump += head;
        // Root CBVs come from push_descriptors; CBVs inside descriptor tables are looked up.
        buffer_range cbs[kMaxCb];
        memcpy(cbs, compute ? st.csCb : st.psCb, sizeof(cbs));
        if (d3d12)
            for (uint32_t i = 0; i < kMaxCb; ++i)
                if (!cbs[i].buffer.handle && decl && decl->HasCb(i))
                    if (const HeapEntry* e = ResolveTableSlot(dev, st, compute, Slot::cbv, i))
                        cbs[i] = {{e->res}, e->cbOffset, UINT64_MAX};
        g_cbDump += DumpConstantBuffers(dev, cbs, decl);
        g_cbDump += "\n";
    }

    for (const auto& b : out) MarkWritten(b.res, key);
    ++g_pass;
}

void OnGeometryDraw(command_list* cmd) {
    auto& st = g_contexts[cmd];
    ++g_geometryRun;
    if (!st.targetsDirty) return;
    st.targetsDirty = false;
    device* dev = cmd->get_device();
    for (uint32_t i = 0; i < st.rtvCount && i < kMaxRtv; ++i)
        if (st.rtv[i].handle) MarkWritten(dev->get_resource_from_view(st.rtv[i]), kGeometryKey);
    if (st.dsv.handle) MarkWritten(dev->get_resource_from_view(st.dsv), kGeometryKey);
}

// The temporal pass with the largest output is taken as the main TAA.
void UpdateMainGuess() {
    const Candidate* best = nullptr;
    for (const auto& [k, c] : g_candidates)
        if (c.lastFrame + 2 >= g_frame && (!best || uint64_t(c.outW) * c.outH > uint64_t(best->outW) * best->outH))
            best = &c;
    if (!best) return;
    if (best->key == g_mainKey && best->inW == g_mainInW && best->inH == g_mainInH && best->outW == g_mainOutW &&
        best->outH == g_mainOutH)
        return;
    g_mainKey = best->key;
    g_mainInW = best->inW;
    g_mainInH = best->inH;
    g_mainOutW = best->outW;
    g_mainOutH = best->outH;
    Log("main TAA guess: %s %016llx, input %ux%u -> output %ux%u (%.1f%% screen percentage)\n%s",
        best->compute ? "CS" : "PS", static_cast<unsigned long long>(best->key), best->inW, best->inH, best->outW,
        best->outH, best->outW ? 100.0 * best->inW / best->outW : 0.0, best->io.c_str());
}

void WriteDump() {
    wchar_t name[64];
    swprintf(name, 64, L"QuarryDLSS_frame_%llu.txt", static_cast<unsigned long long>(g_dumpFrame));
    std::wstring path = g_dir + name;
    if (FILE* f = _wfopen(path.c_str(), L"w")) {
        fprintf(f, "QuarryDLSS %s - frame %llu\nAPI: %s\nadapter: %s\nswap chain: %s\n\n", QDLSS_VERSION,
                static_cast<unsigned long long>(g_dumpFrame), ApiName(g_api), g_adapter.c_str(),
                g_swapchain.c_str());
        fprintf(f, "render scale: %s\n\n", RenderScaleStatus().c_str());
        fprintf(f, "Post-process passes in order (dispatches and full-screen draws). Inputs are\n"
                   "marked with the pass that wrote them this frame; HISTORY marks a texture the\n"
                   "same shader wrote last frame.\n\n");
        for (const auto& l : g_dump) fprintf(f, "%s\n", l.c_str());
        if (g_geometryRun) fprintf(f, "      (%u geometry draws)\n", g_geometryRun);
        fclose(f);
        g_lastDumpFile = Narrow(name);
        Log("wrote %s (%zu entries)", g_lastDumpFile.c_str(), g_dump.size());
    }
    g_dump.clear();
    g_dumpFrame = 0;
}

// ---------------------------------------------------------------- events

void OnInitDevice(device* dev) {
    std::lock_guard lock(g_mutex);
    g_api = dev->get_api();
    g_haveDevice = true;
    Log("device created: %s", ApiName(g_api));
    fg::OnInitDevice(dev);
    if (g_api == device_api::d3d11) {
        auto d3d = reinterpret_cast<ID3D11Device*>(dev->get_native());
        D3D_FEATURE_LEVEL fl = d3d->GetFeatureLevel();
        Log("D3D11 feature level %x.%x", (fl >> 12) & 0xf, (fl >> 8) & 0xf);
        IDXGIDevice* dxgi = nullptr;
        if (SUCCEEDED(d3d->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi)))) {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgi->GetAdapter(&adapter))) {
                DXGI_ADAPTER_DESC ad;
                if (SUCCEEDED(adapter->GetDesc(&ad))) {
                    g_adapter = Narrow(ad.Description);
                    Log("adapter: %s, %llu MB video memory", g_adapter.c_str(),
                        static_cast<unsigned long long>(ad.DedicatedVideoMemory >> 20));
                }
                adapter->Release();
            }
            dxgi->Release();
        }
    } else if (g_api == device_api::d3d12) {
        auto d3d = reinterpret_cast<ID3D12Device*>(dev->get_native());
        const LUID luid = d3d->GetAdapterLuid();
        IDXGIFactory4* factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory)))) {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(factory->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter), reinterpret_cast<void**>(&adapter)))) {
                DXGI_ADAPTER_DESC ad;
                if (SUCCEEDED(adapter->GetDesc(&ad))) {
                    g_adapter = Narrow(ad.Description);
                    Log("adapter: %s, %llu MB video memory", g_adapter.c_str(),
                        static_cast<unsigned long long>(ad.DedicatedVideoMemory >> 20));
                }
                adapter->Release();
            }
            factory->Release();
        }
    }
}

// The back buffer is read on the next ReShade frame, not here: during the game's
// CreateSwapChainForHwnd, ReShade's get_back_buffer crashes (null write in dxgi.dll).
void OnInitSwapchain(swapchain* sc, bool resize) {
    std::lock_guard lock(g_mutex);
    Log("swap chain %s (%s)", resize ? "resized" : "created", ApiName(sc->get_device()->get_api()));
    if (resize) dlss::OnSwapchainResize();
}

void UpdateSwapchainInfo(effect_runtime* rt) {
    auto d = rt->get_device()->get_resource_desc(rt->get_current_back_buffer());
    char buf[160];
    snprintf(buf, sizeof(buf), "%ux%u %s, %u buffers", d.texture.width, d.texture.height, FormatName(d.texture.format),
             rt->get_back_buffer_count());
    g_bbW = d.texture.width;
    g_bbH = d.texture.height;
    if (g_swapchain != buf) {
        g_swapchain = buf;
        Log("back buffer: %s", g_swapchain.c_str());
    }
}

void OnInitPipeline(device*, pipeline_layout, uint32_t count, const pipeline_subobject* subs, pipeline p) {
    PipelineInfo info;
    const shader_desc* parsed[2] = {};  // pixel, compute
    for (uint32_t i = 0; i < count; ++i) {
        auto sd = static_cast<const shader_desc*>(subs[i].data);
        switch (subs[i].type) {
            case pipeline_subobject_type::vertex_shader: info.vs = ShaderKey(sd); break;
            case pipeline_subobject_type::pixel_shader: info.ps = ShaderKey(sd); parsed[0] = sd; break;
            case pipeline_subobject_type::compute_shader: info.cs = ShaderKey(sd); parsed[1] = sd; break;
            default: break;
        }
    }
    if (!info.vs && !info.ps && !info.cs) return;
    std::lock_guard lock(g_mutex);
    g_pipelines[p.handle] = info;
    const uint64_t keys[2] = {info.ps, info.cs};
    for (int i = 0; i < 2; ++i)
        if (keys[i] && parsed[i] && !g_decls.count(keys[i]))
            g_decls[keys[i]] = ParseDxbc(parsed[i]->code, parsed[i]->code_size);
}

void OnDestroyPipeline(device*, pipeline p) {
    std::lock_guard lock(g_mutex);
    g_pipelines.erase(p.handle);
}

void OnDestroyResource(device*, resource r) {
    std::lock_guard lock(g_mutex);
    g_writers.erase(r.handle);
    g_live.erase(r.handle);
    g_mapped.erase(r.handle);
    dlss::OnDestroyResource(r);
    if (g_cbSize.erase(r.handle)) {
        g_cbData.erase(r.handle);
        g_maps.erase(r.handle);
    }
}

// ---------------------------------------------------------------- constant buffer shadows

void OnInitResource(device*, const resource_desc& desc, const subresource_data* init, resource_usage, resource r) {
    if (g_api == device_api::d3d12) {
        std::lock_guard lock(g_mutex);
        g_live.insert(r.handle);
        return;
    }
    if (desc.type != resource_type::buffer || desc.buffer.size == 0 || desc.buffer.size > 65536 ||
        (static_cast<uint32_t>(desc.usage) & static_cast<uint32_t>(resource_usage::constant_buffer)) == 0)
        return;
    std::lock_guard lock(g_mutex);
    g_cbSize[r.handle] = desc.buffer.size;
    auto& data = g_cbData[r.handle];
    data.assign(desc.buffer.size, 0);
    if (init && init->data) memcpy(data.data(), init->data, desc.buffer.size);
}

void WriteShadow(resource r, uint64_t offset, uint64_t size, const void* src) {
    auto it = g_cbSize.find(r.handle);
    if (it == g_cbSize.end() || !src || offset >= it->second) return;
    size = std::min(size, it->second - offset);
    auto& data = g_cbData[r.handle];
    data.resize(it->second);
    memcpy(data.data() + offset, src, size);
}

void OnMapBuffer(device*, resource r, uint64_t offset, uint64_t size, map_access access, void** data) {
    if (access == map_access::read_only || !data || !*data) return;
    std::lock_guard lock(g_mutex);
    if (g_api == device_api::d3d12) {  // UE keeps upload buffers mapped for their whole life
        g_mapped[r.handle] = static_cast<uint8_t*>(*data) - offset;
        return;
    }
    auto it = g_cbSize.find(r.handle);
    if (it == g_cbSize.end() || offset >= it->second) return;
    g_maps[r.handle] = {static_cast<uint8_t*>(*data), offset, std::min(size, it->second - offset)};
}

void OnUnmapBuffer(device*, resource r) {
    std::lock_guard lock(g_mutex);
    if (g_api == device_api::d3d12) {
        g_mapped.erase(r.handle);
        return;
    }
    auto it = g_maps.find(r.handle);
    if (it == g_maps.end()) return;
    WriteShadow(r, it->second.offset, it->second.size, it->second.ptr);
    g_maps.erase(it);
}

bool OnUpdateBuffer(device*, const void* data, resource dest, uint64_t offset, uint64_t size) {
    std::lock_guard lock(g_mutex);
    WriteShadow(dest, offset, size, data);
    return false;
}

bool OnUpdateBufferCommand(command_list*, const void* data, resource dest, uint64_t offset, uint64_t size) {
    std::lock_guard lock(g_mutex);
    WriteShadow(dest, offset, size, data);
    return false;
}

// ---------------------------------------------------------------- D3D12 descriptor events

void OnInitPipelineLayout(device*, uint32_t count, const pipeline_layout_param* params, pipeline_layout layout) {
    std::vector<LayoutParam> out(count);
    for (uint32_t p = 0; p < count; ++p) {
        out[p].type = params[p].type;
        switch (params[p].type) {
            case pipeline_layout_param_type::push_descriptors:
                out[p].ranges.push_back(params[p].push_descriptors);
                break;
            case pipeline_layout_param_type::descriptor_table:
            case pipeline_layout_param_type::push_descriptors_with_ranges:
                for (uint32_t k = 0; k < params[p].descriptor_table.count; ++k)
                    out[p].ranges.push_back(params[p].descriptor_table.ranges[k]);
                break;
            case pipeline_layout_param_type::descriptor_table_with_flags:
            case pipeline_layout_param_type::push_descriptors_with_ranges_and_flags:
                for (uint32_t k = 0; k < params[p].descriptor_table_with_flags.count; ++k)
                    out[p].ranges.push_back(params[p].descriptor_table_with_flags.ranges[k]);
                break;
            default:
                break;
        }
        // D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND: the range follows the previous one.
        uint32_t next = 0;
        for (auto& r : out[p].ranges) {
            if (r.binding == UINT32_MAX) r.binding = next;
            next = r.binding + r.count;
        }
    }
    std::lock_guard lock(g_mutex);
    g_layouts[layout.handle] = std::move(out);
}

// CreateShaderResourceView / CreateUnorderedAccessView / CreateConstantBufferView.
bool OnUpdateDescriptorTables(device* dev, uint32_t count, const descriptor_table_update* updates) {
    if (g_api != device_api::d3d12) return false;
    std::lock_guard lock(g_mutex);
    for (uint32_t k = 0; k < count; ++k) {
        const auto& u = updates[k];
        uint8_t kind;
        if (RangeIs(u.type, Slot::srv)) kind = 1;
        else if (RangeIs(u.type, Slot::uav)) kind = 2;
        else if (RangeIs(u.type, Slot::cbv)) kind = 3;
        else continue;
        if (!u.descriptors || !u.count) continue;
        HeapEntry* base = HeapSlot(dev, u.table, u.binding, true, u.count - 1);
        if (!base) continue;
        for (uint32_t i = 0; i < u.count; ++i) {
            HeapEntry e;
            e.kind = kind;
            if (kind == 3) {
                const auto& r = static_cast<const buffer_range*>(u.descriptors)[i];
                e.res = r.buffer.handle;
                e.cbOffset = r.offset;
            } else {
                const resource_view view = static_cast<const resource_view*>(u.descriptors)[i];
                if (view.handle) {
                    e.res = dev->get_resource_from_view(view).handle;
                    e.viewFormat = static_cast<uint32_t>(dev->get_resource_view_desc(view).format);
                }
            }
            base[i] = e;
            ++g_descriptorWrites;
        }
    }
    return false;
}

// CopyDescriptors(Simple), mostly offline heap -> shader-visible heap.
bool OnCopyDescriptorTables(device* dev, uint32_t count, const descriptor_table_copy* copies) {
    if (g_api != device_api::d3d12) return false;
    std::lock_guard lock(g_mutex);
    for (uint32_t k = 0; k < count; ++k) {
        const auto& c = copies[k];
        if (!c.count) continue;
        HeapEntry* dst = HeapSlot(dev, c.dest_table, c.dest_binding, true, c.count - 1);
        if (!dst) continue;
        // The source heap may be shorter than the copy if part of it was never written.
        descriptor_heap heap = {};
        uint32_t offset = 0;
        dev->get_descriptor_heap_offset(c.source_table, c.source_binding, 0, &heap, &offset);
        auto it = g_heaps.find(heap.handle);
        for (uint32_t i = 0; i < c.count; ++i)
            dst[i] = it != g_heaps.end() && offset + i < it->second.size() ? it->second[offset + i] : HeapEntry{};
        g_descriptorCopies += c.count;
    }
    return false;
}

void OnBindDescriptorTables(command_list* cmd, shader_stage stages, pipeline_layout layout, uint32_t first,
                            uint32_t count, const descriptor_table* tables, uint32_t, const uint32_t*) {
    if (g_api != device_api::d3d12) return;
    const int g = (static_cast<uint32_t>(stages) & static_cast<uint32_t>(shader_stage::compute)) ? 1 : 0;
    std::lock_guard lock(g_mutex);
    auto& st = g_contexts[cmd];
    if (st.layout[g].handle != layout.handle) {  // new root signature: old bindings are gone
        st.layout[g] = layout;
        for (auto& t : st.tables[g]) t = {};
        buffer_range* cbs = g ? st.csCb : st.psCb;
        std::fill(cbs, cbs + kMaxCb, buffer_range{});
        if (g) {
            st.csRootCbvMask = 0;
            st.csRootConstants.clear();
        }
    }
    for (uint32_t i = 0; i < count && first + i < kMaxRootParams; ++i) st.tables[g][first + i] = tables[i];
}

void OnResetCommandList(command_list* cmd) {
    std::lock_guard lock(g_mutex);
    if (auto it = g_contexts.find(cmd); it != g_contexts.end()) it->second = ContextState{};
}

void OnBindPipeline(command_list* cmd, pipeline_stage stages, pipeline p) {
    std::lock_guard lock(g_mutex);
    auto& st = g_contexts[cmd];
    PipelineInfo info;
    if (auto it = g_pipelines.find(p.handle); it != g_pipelines.end()) info = it->second;
    auto has = [&](pipeline_stage s) { return (static_cast<uint32_t>(stages) & static_cast<uint32_t>(s)) != 0; };
    if (has(pipeline_stage::compute_shader)) {
        st.cs = info.cs;
        st.csPipeline = p;
    }
    if (has(pipeline_stage::pixel_shader)) st.ps = info.ps;
    if (has(pipeline_stage::vertex_shader)) st.vs = info.vs;
}

// D3D12 root descriptors (SetCompute/GraphicsRootConstantBufferView): the root
// signature says which register and stage the parameter feeds.
void OnPushRootDescriptor(ContextState& st, bool compute, pipeline_layout layout, uint32_t param,
                          const descriptor_table_update& u) {
    if (u.type != descriptor_type::constant_buffer || !u.descriptors || !u.count) return;
    auto lit = g_layouts.find(layout.handle);
    if (lit == g_layouts.end() || param >= lit->second.size() || lit->second[param].ranges.empty()) return;
    const descriptor_range& r = lit->second[param].ranges[0];
    const buffer_range& range = static_cast<const buffer_range*>(u.descriptors)[0];
    if (compute && param < kMaxRootParams) {
        st.csRootCbv[param] = range;
        st.csRootCbvMask |= 1ull << param;
    }
    if (r.dx_register_space != 0 || !Visible(r.visibility, compute) || r.dx_register_index >= kMaxCb) return;
    (compute ? st.csCb : st.psCb)[r.dx_register_index] = range;
}

void OnPushDescriptors(command_list* cmd, shader_stage stages, pipeline_layout layout, uint32_t param,
                       const descriptor_table_update& u) {
    const bool cs = (static_cast<uint32_t>(stages) & static_cast<uint32_t>(shader_stage::compute)) != 0;
    const bool ps = (static_cast<uint32_t>(stages) & static_cast<uint32_t>(shader_stage::pixel)) != 0;
    if (!cs && !ps) return;
    resource_view* dst = nullptr;
    uint32_t cap = 0;
    std::lock_guard lock(g_mutex);
    auto& st = g_contexts[cmd];
    if (g_api == device_api::d3d12) {
        OnPushRootDescriptor(st, cs, layout, param, u);
        return;
    }
    if (u.type == descriptor_type::constant_buffer) {
        buffer_range* cbs = cs ? st.csCb : st.psCb;
        auto ranges = static_cast<const buffer_range*>(u.descriptors);
        for (uint32_t i = 0; i < u.count && u.binding + i < kMaxCb; ++i)
            cbs[u.binding + i] = ranges ? ranges[i] : buffer_range{};
        return;
    }
    switch (u.type) {
        case descriptor_type::shader_resource_view:
        case descriptor_type::buffer_shader_resource_view:
            dst = cs ? st.csSrv : st.psSrv;
            cap = kMaxSrv;
            break;
        case descriptor_type::unordered_access_view:
        case descriptor_type::buffer_unordered_access_view:
            if (!cs) return;
            dst = st.csUav;
            cap = kMaxUav;
            break;
        default: return;
    }
    auto views = static_cast<const resource_view*>(u.descriptors);
    for (uint32_t i = 0; i < u.count && u.binding + i < cap; ++i) dst[u.binding + i] = views ? views[i] : resource_view{};
}

void OnBindTargets(command_list* cmd, uint32_t count, const resource_view* rtvs, resource_view dsv) {
    std::lock_guard lock(g_mutex);
    auto& st = g_contexts[cmd];
    st.rtvCount = std::min(count, kMaxRtv);
    for (uint32_t i = 0; i < kMaxRtv; ++i) st.rtv[i] = i < st.rtvCount ? rtvs[i] : resource_view{};
    st.dsv = dsv;
    st.targetsDirty = true;
}

void OnBindViewports(command_list* cmd, uint32_t first, uint32_t count, const viewport* vps) {
    if (first != 0 || count == 0) return;
    std::lock_guard lock(g_mutex);
    g_contexts[cmd].vp = vps[0];
}

// Unreal draws post-process passes as one triangle or a two-triangle quad.
bool IsFullscreen(uint32_t count, uint32_t instances) { return count <= 6 && instances <= 1; }

bool OnDraw(command_list* cmd, uint32_t vertices, uint32_t instances, uint32_t, uint32_t) {
    std::lock_guard lock(g_mutex);
    if (IsFullscreen(vertices, instances) && g_contexts[cmd].rtvCount)
        OnPass(cmd, false, "draw");
    else
        OnGeometryDraw(cmd);
    return false;
}

bool OnDrawIndexed(command_list* cmd, uint32_t indices, uint32_t instances, uint32_t, int32_t, uint32_t) {
    std::lock_guard lock(g_mutex);
    if (IsFullscreen(indices, instances) && g_contexts[cmd].rtvCount)
        OnPass(cmd, false, "draw");
    else
        OnGeometryDraw(cmd);
    return false;
}

// ---------------------------------------------------------------- DLSS in place of TAAU

// The game's TAAU compute shaders (TemporalAA.usf with upsampling), by shader key. UE 4.26
// picks the permutation by screen percentage band, so each quality mode can use another.
constexpr uint64_t kTaauShaders[] = {
    0x6435b4eb8190e22bull,  // 50-71%: Quality, Balanced, Performance (2026-10-15)
    0x2f606fcaa114ea67ull,  // 71-100%: Ultra Quality 77% (and the game's own 100% TAA)
};

// A known permutation, or else the pass the probe currently takes for the main TAA (a
// permutation not seen yet, e.g. Ultra Performance). Either way BuildDlssFrame only accepts
// it when it actually upscales, so a 100% TAA or a same-shaped denoiser is never replaced.
bool IsTaau(uint64_t key) {
    for (uint64_t k : kTaauShaders)
        if (k == key) return true;
    // The guess must itself upscale (by texture sizes): at 100% the guess can be the SSGI
    // denoiser b569efd647217872, whose constants mean something else.
    if (key && key == g_mainKey && g_mainOutW > g_mainInW) {
        static uint64_t logged = 0;
        if (logged != key) {
            logged = key;
            Log("dlss: main TAA %016llx is not a known TAAU permutation; trying it as one",
                static_cast<unsigned long long>(key));
        }
        return true;
    }
    return false;
}

// DLSS must be reset on a camera cut or it blends the previous shot into the new one.
// The TAA's bCameraCut (cb0 row 0.z) may not be raised by the game's cinematic cameras,
// so a jump of the camera (View.WorldCameraOrigin, cb1 row 67, UE cm) of more than 2 m
// or a turn (View forward, cb1 row 18) of more than 15 degrees in one frame also counts.
float g_prevOrigin[3] = {}, g_prevForward[3] = {};
bool g_haveCamera = false;
uint32_t g_cutsLogged = 0;

bool DetectCameraCut(bool flag, const float* origin, const float* forward) {
    float dist = 0, dot = 1;
    if (g_haveCamera) {
        const float dx = origin[0] - g_prevOrigin[0], dy = origin[1] - g_prevOrigin[1], dz = origin[2] - g_prevOrigin[2];
        dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        dot = forward[0] * g_prevForward[0] + forward[1] * g_prevForward[1] + forward[2] * g_prevForward[2];
    }
    memcpy(g_prevOrigin, origin, sizeof(g_prevOrigin));
    memcpy(g_prevForward, forward, sizeof(g_prevForward));
    const bool jump = g_haveCamera && (dist > 200.0f || dot < 0.966f);
    g_haveCamera = true;
    if ((flag || jump) && ++g_cutsLogged <= 200)
        Log("dlss: reset at frame %llu: %s%s (camera moved %.0f cm, turned %.1f deg)",
            static_cast<unsigned long long>(g_frame), flag ? "game camera-cut flag" : "",
            jump ? (flag ? " + camera jump" : "camera jump") : "", dist,
            std::acos(std::max(-1.0f, std::min(1.0f, dot))) * 57.29578f);
    return flag || jump;
}

// Bindings and constants of one TAAU dispatch (FINDINGS.md, "TAAU path at 67%"):
// t1 colour, t2 depth, t3 velocity, u0 output; cb0 row 27 render rect, row 24 output
// rect, row 28.xy jitter, row 0.z camera cut; cb1 (View) rows 122-125 ClipToPrevClip,
// row 135.y PreExposure.
bool BuildDlssFrame(command_list* cmd, const ContextState& st, dlss::Frame& f, std::string& why) {
    device* dev = cmd->get_device();
    const HeapEntry* color = ResolveTableSlot(dev, st, true, Slot::srv, 1);
    const HeapEntry* depth = ResolveTableSlot(dev, st, true, Slot::srv, 2);
    const HeapEntry* velocity = ResolveTableSlot(dev, st, true, Slot::srv, 3);
    const HeapEntry* output = ResolveTableSlot(dev, st, true, Slot::uav, 0);
    const HeapEntry* exposure = ResolveTableSlot(dev, st, true, Slot::srv, 0);
    for (const HeapEntry* e : {color, depth, velocity, output})
        if (!e || !e->res || !IsLive(e->res)) {
            why = "a TAAU input or output could not be resolved";
            return false;
        }
    std::vector<uint8_t> cb0, cb1;
    if (!ReadConstants(dev, st.csCb[0], 29 * 16, cb0) || cb0.size() < 29 * 16 ||
        !ReadConstants(dev, st.csCb[1], 136 * 16, cb1) || cb1.size() < 136 * 16) {
        why = "the TAAU constants could not be read";
        return false;
    }
    auto row = [](const std::vector<uint8_t>& v, int r) { return reinterpret_cast<const float*>(v.data() + r * 16); };
    uint32_t cameraCut;
    memcpy(&cameraCut, row(cb0, 0) + 2, 4);

    f.cmd = cmd;
    f.color = {color->res};
    f.depth = {depth->res};
    f.velocity = {velocity->res};
    f.output = {output->res};
    if (exposure && exposure->res && IsLive(exposure->res)) {
        const auto d = dev->get_resource_desc({exposure->res});
        if (d.texture.width == 1 && d.texture.height == 1 && d.texture.format == format::r32g32b32a32_float)
            f.exposure = {exposure->res};  // UE's eye adaptation
    }
    f.renderW = static_cast<uint32_t>(row(cb0, 27)[0]);
    f.renderH = static_cast<uint32_t>(row(cb0, 27)[1]);
    f.outX = static_cast<uint32_t>(row(cb0, 24)[0]);
    f.outY = static_cast<uint32_t>(row(cb0, 24)[1]);
    f.outW = static_cast<uint32_t>(row(cb0, 24)[2]) - f.outX;
    f.outH = static_cast<uint32_t>(row(cb0, 24)[3]) - f.outY;
    f.jitterX = row(cb0, 28)[0];
    f.jitterY = row(cb0, 28)[1];
    f.reset = DetectCameraCut(cameraCut != 0, row(cb1, 67), row(cb1, 18));
    memcpy(f.clipToPrevClip, row(cb1, 122), sizeof(f.clipToPrevClip));
    f.preExposure = row(cb1, 135)[1];
    if (!(f.preExposure > 0.0f) || f.preExposure > 1e30f) f.preExposure = 1.0f;
    memcpy(f.viewToClip, row(cb1, 28), sizeof(f.viewToClip));
    memcpy(f.cameraPos, row(cb1, 67), sizeof(f.cameraPos));
    memcpy(f.cameraFwd, row(cb1, 18), sizeof(f.cameraFwd));
    f.backBufferW = g_bbW;
    f.backBufferH = g_bbH;

    // During loading the game binds 1x1 placeholders (e.g. for velocity): every input
    // must cover the render rect.
    const auto colorDesc = dev->get_resource_desc(f.color);
    const auto depthDesc = dev->get_resource_desc(f.depth);
    const auto velocityDesc = dev->get_resource_desc(f.velocity);
    const auto outDesc = dev->get_resource_desc(f.output);
    const uint32_t minW = std::min({colorDesc.texture.width, depthDesc.texture.width, velocityDesc.texture.width});
    const uint32_t minH = std::min({colorDesc.texture.height, depthDesc.texture.height, velocityDesc.texture.height});
    // Specific reasons, without per-frame values like jitter, so the log only records changes.
    char buf[200];
    if (f.renderW == 0 || f.renderH == 0 || f.outW == 0 || f.outH == 0) {
        snprintf(buf, sizeof(buf), "empty rect (render %ux%u, output %ux%u)", f.renderW, f.renderH, f.outW, f.outH);
    } else if (f.renderW >= f.outW || f.renderH >= f.outH) {
        snprintf(buf, sizeof(buf), "not upscaling (render %ux%u, output %ux%u)", f.renderW, f.renderH, f.outW, f.outH);
    } else if (f.renderW > minW || f.renderH > minH) {
        snprintf(buf, sizeof(buf), "an input is smaller than the %ux%u render rect (colour %ux%u, depth %ux%u, velocity %ux%u)",
                 f.renderW, f.renderH, colorDesc.texture.width, colorDesc.texture.height, depthDesc.texture.width,
                 depthDesc.texture.height, velocityDesc.texture.width, velocityDesc.texture.height);
    } else if (f.outX + f.outW > outDesc.texture.width || f.outY + f.outH > outDesc.texture.height) {
        snprintf(buf, sizeof(buf), "output rect %u,%u %ux%u outside the %ux%u output texture", f.outX, f.outY, f.outW,
                 f.outH, outDesc.texture.width, outDesc.texture.height);
    } else if (std::abs(f.jitterX) > 0.5f || std::abs(f.jitterY) > 0.5f) {
        snprintf(buf, sizeof(buf), "jitter out of range");
    } else {
        return true;
    }
    why = buf;
    return false;
}

// NGX bound its own heaps, root signature and pipeline behind ReShade's back. Our
// prepare pass left ReShade tracking its own transient heaps and our root signature,
// so re-binding the game's tables through ReShade re-issues SetDescriptorHeaps and
// SetComputeRootSignature, and then the game's tables, root CBVs, root constants and
// pipeline go back exactly as the game left them. Graphics tables are re-bound too
// (same values): if the compute root signature has no sampler table, that is what
// puts the game's sampler heap back.
void RestoreComputeState(command_list* cmd, const ContextState& st) {
    if (st.layout[0].handle)
        for (uint32_t p = 0; p < kMaxRootParams; ++p)
            if (st.tables[0][p].handle)
                cmd->bind_descriptor_tables(shader_stage::all_graphics, st.layout[0], p, 1, &st.tables[0][p]);
    const pipeline_layout layout = st.layout[1];
    for (uint32_t p = 0; p < kMaxRootParams; ++p)
        if (st.tables[1][p].handle) cmd->bind_descriptor_tables(shader_stage::all_compute, layout, p, 1, &st.tables[1][p]);
    for (uint32_t p = 0; p < kMaxRootParams; ++p)
        if (st.csRootCbvMask & (1ull << p))
            cmd->push_descriptors(shader_stage::all_compute, layout, p,
                                  descriptor_table_update{{}, 0, 0, 1, descriptor_type::constant_buffer, &st.csRootCbv[p]});
    for (const auto& [p, values] : st.csRootConstants)
        cmd->push_constants(shader_stage::all_compute, layout, p, 0, static_cast<uint32_t>(values.size()), values.data());
    if (st.csPipeline.handle) cmd->bind_pipeline(pipeline_stage::compute_shader, st.csPipeline);
}

// A camera cut must reach DLSS even if the cut frame itself falls back to the game's
// TAAU (the cut flag is only set on that one frame): it stays pending until DLSS runs.
bool g_pendingCut = false;
std::string g_lastSkipWhy;
uint64_t g_skipCount = 0;

void LogSkip(const std::string& why) {
    dlss::CancelLightingHistory();
    ++g_skipCount;
    // Every change of reason, and every 600th repeat, so cut frames can't hide.
    if (why != g_lastSkipWhy || g_skipCount % 600 == 0)
        Log("dlss: game TAAU used at frame %llu (%llu fallbacks so far): %s", static_cast<unsigned long long>(g_frame),
            static_cast<unsigned long long>(g_skipCount), why.c_str());
    g_lastSkipWhy = why;
}

bool OnDispatch(command_list* cmd, uint32_t x, uint32_t y, uint32_t z) {
    std::lock_guard lock(g_mutex);
    char what[48];
    snprintf(what, sizeof(what), "dispatch %ux%ux%u", x, y, z);
    OnPass(cmd, true, what);

    auto& st = g_contexts[cmd];
    if (g_api != device_api::d3d12 || !IsTaau(st.cs)) return false;
    dlss::Frame f;
    std::string why;
    const bool built = BuildDlssFrame(cmd, st, f, why);
    // The game's pre-exposure, with DLSS on or off: is a jump at a cut the game's own?
    static float loggedPre = 0.0f;
    if (built && dlss::g_options.verboseLog && (f.preExposure > loggedPre * 1.25f || f.preExposure < loggedPre * 0.8f)) {
        Log("pre-exposure %.4g at frame %llu (DLSS %s)%s", f.preExposure, static_cast<unsigned long long>(g_frame),
            dlss::g_options.enabled ? "on" : "off", f.reset ? ", camera cut" : "");
        loggedPre = f.preExposure;
    }
    if (!dlss::g_options.enabled) return false;
    g_pendingCut = g_pendingCut || f.reset;
    if (!built) {
        LogSkip(why);
        return false;
    }
    f.reset = g_pendingCut;
    f.frame = g_frame;
    // Snapshot first: whatever DLSS records must not be mistaken for the game's state.
    const ContextState saved = st;
    const dlss::Result r = dlss::Evaluate(f);
    if (r != dlss::Result::skipped) RestoreComputeState(cmd, saved);
    if (r == dlss::Result::done) {
        g_pendingCut = false;
        g_lastSkipWhy.clear();
    } else {
        LogSkip(r == dlss::Result::failed ? "DLSS evaluate failed" : "DLSS not ready for this size");
    }
    return r == dlss::Result::done;  // true skips the game's TAAU dispatch
}

void OnPushConstants(command_list* cmd, shader_stage stages, pipeline_layout, uint32_t param, uint32_t first,
                     uint32_t count, const void* values) {
    if (g_api != device_api::d3d12 || !(static_cast<uint32_t>(stages) & static_cast<uint32_t>(shader_stage::compute)))
        return;
    std::lock_guard lock(g_mutex);
    auto& v = g_contexts[cmd].csRootConstants[param];
    if (v.size() < first + count) v.resize(first + count);
    memcpy(v.data() + first, values, count * 4);
}

void OnDestroyDevice(device* dev) {
    std::lock_guard lock(g_mutex);
    dlss::Shutdown(dev);
}

bool OnCopyResource(command_list* cmd, resource src, resource dst) {
    std::lock_guard lock(g_mutex);
    if (auto it = g_writers.find(src.handle); it != g_writers.end()) g_writers[dst.handle] = it->second;
    if (g_dumpFrame && g_frame == g_dumpFrame) {
        auto d = cmd->get_device()->get_resource_desc(dst);
        char line[160];
        snprintf(line, sizeof(line), "      copy %llx -> %llx (%ux%u %s)", static_cast<unsigned long long>(src.handle),
                 static_cast<unsigned long long>(dst.handle), d.texture.width, d.texture.height,
                 FormatName(d.texture.format));
        g_dump.push_back(line);
    }
    return false;
}

void WriteCbDump() {
    wchar_t name[64];
    swprintf(name, 64, L"QuarryDLSS_cbuffers_%llu.txt", static_cast<unsigned long long>(g_cbDumpFirst));
    std::wstring path = g_dir + name;
    if (FILE* f = _wfopen(path.c_str(), L"w")) {
        fprintf(f, "QuarryDLSS %s - constant buffers, frames %llu-%llu\nswap chain: %s\n\n", QDLSS_VERSION,
                static_cast<unsigned long long>(g_cbDumpFirst), static_cast<unsigned long long>(g_cbDumpLast),
                g_swapchain.c_str());
        fprintf(f, "render scale: %s\n\n", RenderScaleStatus().c_str());
        fprintf(f, "Every temporal pass with an output of at least a quarter of the back buffer.\n"
                   "Rows are float4: values, then raw bits. Jitter shows as small values that\n"
                   "change every frame and repeat with the TAA sample count.\n\n");
        fputs(g_cbDump.c_str(), f);
        fclose(f);
        g_lastCbDumpFile = Narrow(name);
        Log("wrote %s (%zu bytes)", g_lastCbDumpFile.c_str(), g_cbDump.size());
    }
    g_cbDump.clear();
    g_cbDumpFirst = g_cbDumpLast = 0;
}

void OnPresent(command_queue*, swapchain*, const rect*, const rect*, uint32_t, const rect*) {
    std::lock_guard lock(g_mutex);
    // With DLSS-G below ReShade's caller (factory hook), generated frames reach this too.
    if (!fg::IsRealPresent()) return;
    if (g_dumpFrame && g_frame == g_dumpFrame) WriteDump();
    if (g_cbDumpFirst && g_frame == g_cbDumpLast) WriteCbDump();
    UpdateMainGuess();
    ++g_frame;
    g_pass = 0;
    g_geometryRun = 0;
}

void RequestDump() {
    if (!g_dumpFrame) {
        g_dumpFrame = g_frame + 1;
        g_dump.clear();
    }
}

void RequestCbDump() {
    if (!g_cbDumpFirst) {
        g_cbDumpFirst = g_frame + 1;
        g_cbDumpLast = g_frame + kCbDumpFrames;
        g_cbDump.clear();
    }
}

// ---------------------------------------------------------------- forced render scale
//
// The game ignores r.ScreenPercentage from Engine.ini, so the probe writes the
// cvar data directly every frame (both the game-thread and render-thread copies),
// together with r.TemporalAA.Upsampling=1, to make the main TAA upsample (TAAU).

enum class CvarState { untried, waiting, ready, failed };
CvarState g_cvarState = CvarState::untried;
void** g_cvarSingleton = nullptr;
float* g_screenPercentage = nullptr;  // TConsoleVariableData<float>::ShadowedValue[2]
int32_t* g_taaUpsampling = nullptr;   // TConsoleVariableData<int32>::ShadowedValue[2]
float g_originalPercentage = 100.0f;
int32_t g_originalUpsampling = 0;
bool g_forceScale = true;
bool g_forcing = false;  // values currently overwritten
float g_forcePercentage = 66.7f;
bool g_keepScaleWhenOff = false;  // diagnostics: keep the forced scale with DLSS off (TAAU comparison)
bool g_debugHotkeys = false;      // Ctrl+F8 / F9 / F10 captures and dumps

// Quality modes: render scale per axis, as DLSS defines them. "Custom" = the Advanced slider.
struct QualityMode {
    const char* name;
    const char* key;  // in ReShade.ini, stable across versions
    float percentage;
};
const QualityMode kQualityModes[] = {
    {"Ultra Quality (77%)", "UltraQuality", 77.0f}, {"Quality (67%)", "Quality", 66.7f},
    {"Balanced (58%)", "Balanced", 58.0f},          {"Performance (50%)", "Performance", 50.0f},
    {"Ultra Performance (33%)", "UltraPerformance", 33.3f}, {"Custom", "Custom", 0.0f}};
constexpr int kCustomQuality = 5;
int g_quality = 1;  // Quality
uint32_t g_foreignWrites = 0;  // times the game changed a forced value

void FindCvars() {
    if (g_cvarState == CvarState::untried) {
        g_cvarSingleton = cvars::FindConsoleManagerSingleton(reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr)));
        if (!g_cvarSingleton) {
            g_cvarState = CvarState::failed;
            Log("cvars: IConsoleManager::Singleton not found (no lea of L\"r.ScreenPercentage\" after mov rcx,[rip+x])");
            return;
        }
        Log("cvars: IConsoleManager::Singleton at exe+0x%llx",
            static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(g_cvarSingleton) -
                                            reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr))));
        g_cvarState = CvarState::waiting;
    }
    void* manager = *g_cvarSingleton;
    if (!manager) return;  // engine not up yet
    auto sp = static_cast<float*>(cvars::FindData(manager, L"r.ScreenPercentage", true));
    auto up = static_cast<int32_t*>(cvars::FindData(manager, L"r.TemporalAA.Upsampling", false));
    // Sanity check before ever writing: a wrong vtable layout would hand back something else.
    if (!sp || !up || !(sp[0] >= 1.0f && sp[0] <= 400.0f) || up[0] < 0 || up[0] > 2) {
        g_cvarState = CvarState::failed;
        Log("cvars: lookup failed or implausible (r.ScreenPercentage %p = %g, r.TemporalAA.Upsampling %p = %d); "
            "not forcing anything",
            static_cast<void*>(sp), sp ? sp[0] : 0.0, static_cast<void*>(up), up ? up[0] : 0);
        return;
    }
    g_screenPercentage = sp;
    g_taaUpsampling = up;
    g_originalPercentage = sp[0];
    g_originalUpsampling = up[0];
    g_cvarState = CvarState::ready;
    Log("cvars: r.ScreenPercentage = %g / %g (game / render thread), r.TemporalAA.Upsampling = %d / %d", sp[0], sp[1],
        up[0], up[1]);
}

void ApplyRenderScale() {
    if (g_cvarState == CvarState::untried || g_cvarState == CvarState::waiting) FindCvars();
    if (g_cvarState != CvarState::ready) return;
    float* sp = g_screenPercentage;
    int32_t* up = g_taaUpsampling;
    // DLSS off: the game's own render scale and TAA, unless kept for a comparison.
    if (!g_forceScale || !(dlss::g_options.enabled || g_keepScaleWhenOff)) {
        if (g_forcing) {  // give the game its own values back
            sp[0] = sp[1] = g_originalPercentage;
            up[0] = up[1] = g_originalUpsampling;
            g_forcing = false;
            Log("cvars: restored r.ScreenPercentage %g, r.TemporalAA.Upsampling %d", g_originalPercentage,
                g_originalUpsampling);
        }
        return;
    }
    static float written = 0.0f;  // what we last wrote: a quality change isn't the game's doing
    if (g_forcing && (sp[0] != written || up[0] != 1) && ++g_foreignWrites <= 10)
        Log("cvars: the game changed a forced value: r.ScreenPercentage %g / %g, r.TemporalAA.Upsampling %d / %d%s",
            sp[0], sp[1], up[0], up[1], g_foreignWrites == 10 ? " (not logging further changes)" : "");
    if (!g_forcing) Log("cvars: forcing r.ScreenPercentage %g and r.TemporalAA.Upsampling 1", g_forcePercentage);
    sp[0] = sp[1] = written = g_forcePercentage;
    up[0] = up[1] = 1;
    g_forcing = true;
}

// Test switches for effects that read the previous frame's TAA output (= DLSS output)
// back into this frame's lighting: UE 4.26 SSGI (enabled by NativeSSGI_P.pak) and SSR
// (quality 4 from BetterReflections_P.pak). That loop has no history clamp under DLSS.
struct IntOverride {
    const wchar_t* name;
    const char* label;
    int32_t forced;
    bool enabled = false;
    bool looked = false;
    bool active = false;
    int32_t* data = nullptr;  // TConsoleVariableData<int32>::ShadowedValue[2]
    int32_t original = 0;
};
IntOverride g_overrides[] = {
    {L"r.SSGI.Enable", "Disable SSGI (r.SSGI.Enable 0, test)", 0},
    {L"r.SSR.Quality", "Disable SSR (r.SSR.Quality 0, test)", 0},
};

void ApplyOverrides() {
    if (g_cvarState != CvarState::ready) return;
    for (auto& o : g_overrides) {
        if (!o.looked) {
            o.looked = true;
            auto data = static_cast<int32_t*>(cvars::FindData(*g_cvarSingleton, o.name, false));
            if (data && data[0] >= 0 && data[0] <= 16) {
                o.data = data;
                Log("cvars: %ls = %d / %d", o.name, data[0], data[1]);
            } else {
                Log("cvars: %ls not found or implausible", o.name);
            }
        }
        if (!o.data) continue;
        if (o.enabled) {
            if (!o.active) {
                o.original = o.data[0];
                o.active = true;
                Log("cvars: forcing %ls %d (was %d)", o.name, o.forced, o.original);
            }
            o.data[0] = o.data[1] = o.forced;
        } else if (o.active) {
            o.data[0] = o.data[1] = o.original;
            o.active = false;
            Log("cvars: restored %ls %d", o.name, o.original);
        }
    }
}

std::string RenderScaleStatus() {
    char buf[160];
    if (g_cvarState != CvarState::ready) return "console variables not found";
    snprintf(buf, sizeof(buf), "r.ScreenPercentage %.1f, r.TemporalAA.Upsampling %d (%s; game's own %.0f, %d)",
             g_screenPercentage[0], g_taaUpsampling[0], g_forcing ? "forced by the probe" : "not forced",
             g_originalPercentage, g_originalUpsampling);
    return buf;
}

void OnReshadePresent(effect_runtime* rt) {
    std::lock_guard lock(g_mutex);
    UpdateSwapchainInfo(rt);
    ApplyRenderScale();
    ApplyOverrides();
    fg::g_options.verboseLog = dlss::g_options.verboseLog;
    dlss::Tick(rt);
    if (g_debugHotkeys && rt->is_key_down(VK_CONTROL)) {
        if (rt->is_key_pressed(VK_F9)) RequestDump();  // not F11: UE toggles fullscreen on it
        if (rt->is_key_pressed(VK_F10)) RequestCbDump();
        if (rt->is_key_pressed(VK_F8)) dlss::RequestCapture(rt->is_key_down(VK_SHIFT));
    }
}

// ---- settings, saved in ReShade.ini under [QuarryDLSS] ----

constexpr char kConfigSection[] = "QuarryDLSS";

void LoadSettings() {
    auto& o = dlss::g_options;
    reshade::get_config_value(nullptr, kConfigSection, "Enabled", o.enabled);
    char key[32] = {};
    size_t keySize = sizeof(key);
    int legacy = 0;
    if (reshade::get_config_value(nullptr, kConfigSection, "Quality", key, &keySize)) {
        for (int i = 0; i <= kCustomQuality; ++i)
            if (strcmp(key, kQualityModes[i].key) == 0) g_quality = i;
    } else if (reshade::get_config_value(nullptr, kConfigSection, "QualityMode", legacy) && legacy >= 0 && legacy <= 4) {
        g_quality = legacy + 1;  // 0.6.0 saved an index into a list without Ultra Quality
    }
    float custom = g_forcePercentage;
    if (reshade::get_config_value(nullptr, kConfigSection, "CustomScale", custom) && custom >= 33.0f && custom <= 100.0f)
        g_forcePercentage = custom;
    if (g_quality != kCustomQuality) g_forcePercentage = kQualityModes[g_quality].percentage;
    unsigned int preset = o.preset;
    if (reshade::get_config_value(nullptr, kConfigSection, "Preset", preset)) o.preset = preset;
    // New key in 1.1.1: 1.0/1.1 saved the old default (on) with every setting change, so the
    // old key can't tell a choice from the default. Everyone starts with it off.
    reshade::get_config_value(nullptr, kConfigSection, "StabiliseScreenSpaceLighting", o.clampLightingHistory);
    Log("settings: DLSS %s, %s, render scale %.1f%%, preset %u, stabilise lighting %d", o.enabled ? "on" : "off",
        kQualityModes[g_quality].name, g_forcePercentage, o.preset, o.clampLightingHistory);
}

void SaveSettings() {
    const auto& o = dlss::g_options;
    reshade::set_config_value(nullptr, kConfigSection, "Enabled", o.enabled);
    reshade::set_config_value(nullptr, kConfigSection, "Quality", kQualityModes[g_quality].key);
    reshade::set_config_value(nullptr, kConfigSection, "CustomScale", g_forcePercentage);
    reshade::set_config_value(nullptr, kConfigSection, "Preset", static_cast<unsigned int>(o.preset));
    reshade::set_config_value(nullptr, kConfigSection, "StabiliseScreenSpaceLighting", o.clampLightingHistory);
}

// One plain line for the overlay.
std::string FriendlyStatus() {
    if (!dlss::g_options.enabled) return "Off: the game's own anti-aliasing at native resolution.";
    if (g_haveDevice && g_api != device_api::d3d12)
        return "Not available: DLSS needs DirectX 12. Add -dx12 to the game's launch options.";
    const std::string st = dlss::Status();
    unsigned rw, rh, ow, oh;
    if (sscanf(st.c_str(), "running: %ux%u -> %ux%u", &rw, &rh, &ow, &oh) == 4) {
        char buf[96];
        snprintf(buf, sizeof(buf), "Active: %ux%u upscaled to %ux%u.", rw, rh, ow, oh);
        return buf;
    }
    if (st.rfind("ready", 0) == 0 || st.rfind("waiting", 0) == 0) return "Starting: load into the game.";
    return "Not active: " + st + " (see QuarryDLSS.log).";
}

// Everything a bug report or a test run needs; collapsed by default.
void DrawDiagnostics() {
    ImGui::Text("API: %s", g_haveDevice ? ApiName(g_api) : "(no device yet)");
    if (!g_adapter.empty()) ImGui::Text("Adapter: %s", g_adapter.c_str());
    ImGui::Text("Swap chain: %s, frame %llu", g_swapchain.c_str(), static_cast<unsigned long long>(g_frame));
    ImGui::Text("DLSS: %s", dlss::Status().c_str());
    switch (g_cvarState) {
        case CvarState::ready:
            ImGui::Text("Now: r.ScreenPercentage %.1f, r.TemporalAA.Upsampling %d (game's own: %.0f, %d)",
                        g_screenPercentage[0], g_taaUpsampling[0], g_originalPercentage, g_originalUpsampling);
            if (g_foreignWrites) ImGui::Text("The game changed the forced values %u time(s), see the log.", g_foreignWrites);
            ImGui::Checkbox("Force the render scale", &g_forceScale);
            ImGui::Checkbox("Keep the render scale with DLSS off (compare with the game's TAAU)", &g_keepScaleWhenOff);
            for (auto& o : g_overrides)
                if (o.data) ImGui::Checkbox(o.label, &o.enabled);
            break;
        case CvarState::failed: ImGui::TextUnformatted("Render scale: console variables not found (see log)."); break;
        default: ImGui::TextUnformatted("Render scale: waiting for the engine..."); break;
    }
    ImGui::Separator();
    auto& o = dlss::g_options;
    ImGui::Checkbox("Pass the game's pre-exposure to DLSS", &o.passPreExposure);
    ImGui::Checkbox("Use the game's exposure, not DLSS auto exposure", &o.gameExposure);
    ImGui::Checkbox("Flip jitter X", &o.flipJitterX);
    ImGui::SameLine();
    ImGui::Checkbox("Flip jitter Y", &o.flipJitterY);
    ImGui::SameLine();
    ImGui::Checkbox("Motion vectors include jitter", &o.mvJittered);
    ImGui::Checkbox("Invert motion vectors (test)", &o.invertMotion);
    ImGui::Checkbox("Ignore game velocity, camera motion only (test)", &o.ignoreVelocity);
    ImGui::Text("Lighting history copies: %llu", static_cast<unsigned long long>(dlss::LightingHistoryPatches()));
    ImGui::Separator();
    ImGui::Checkbox("Verbose log (motion statistics, pre-exposure changes)", &o.verboseLog);
    ImGui::Checkbox("Log brightness every frame (input / DLSS / clamped luma)", &o.logBrightness);
    ImGui::Checkbox("Debug hotkeys: Ctrl+F9 frame dump, Ctrl+F10 constants, Ctrl+F8 texture capture", &g_debugHotkeys);
    if (ImGui::Button(g_dumpFrame ? "Capturing..." : "Dump next frame")) RequestDump();
    ImGui::SameLine();
    if (ImGui::Button(g_cbDumpFirst ? "Capturing constants..." : "Dump TAA constants (8 frames)")) RequestCbDump();
    if (ImGui::Button("Capture DLSS textures")) dlss::RequestCapture();
    ImGui::SameLine();
    if (ImGui::Button("Capture 45 frames into the next shot")) dlss::RequestCapture(true);
    if (!g_lastDumpFile.empty()) ImGui::Text("Last dump: %s", g_lastDumpFile.c_str());
    if (!g_lastCbDumpFile.empty()) ImGui::Text("Last constants dump: %s", g_lastCbDumpFile.c_str());
    if (!dlss::CaptureStatus().empty()) ImGui::Text("Capture: %s", dlss::CaptureStatus().c_str());
    ImGui::Separator();
    if (g_mainKey)
        ImGui::Text("Main TAA: %016llx, %ux%u -> %ux%u (%.1f%%)", static_cast<unsigned long long>(g_mainKey), g_mainInW,
                    g_mainInH, g_mainOutW, g_mainOutH, g_mainOutW ? 100.0 * g_mainInW / g_mainOutW : 0.0);
    std::vector<const Candidate*> list;
    for (const auto& [k, c] : g_candidates) list.push_back(&c);
    std::sort(list.begin(), list.end(), [](const Candidate* a, const Candidate* b) {
        return uint64_t(a->outW) * a->outH > uint64_t(b->outW) * b->outH;
    });
    ImGui::Text("Temporal passes (%zu):", list.size());
    for (const Candidate* c : list) {
        bool live = c->lastFrame + 2 >= g_frame;
        ImGui::Text("%s %s %016llx  %ux%u -> %ux%u  hits %llu", live ? "*" : " ", c->compute ? "CS" : "PS",
                    static_cast<unsigned long long>(c->key), c->inW, c->inH, c->outW, c->outH,
                    static_cast<unsigned long long>(c->hits));
    }
    if (g_api == device_api::d3d12)
        ImGui::Text("D3D12: %zu root signatures, %zu heaps, %llu descriptor writes, %llu copied, %zu mapped buffers",
                    g_layouts.size(), g_heaps.size(), static_cast<unsigned long long>(g_descriptorWrites),
                    static_cast<unsigned long long>(g_descriptorCopies), g_mapped.size());
    ImGui::Separator();
    ImGui::Text("Frame generation: %s", fg::Status().c_str());
    ImGui::TextWrapped("Pacing: %s", fg::Pacing().c_str());
    if (ImGui::Checkbox("Streamline copies motion vectors and depth when tagged", &fg::g_options.copyTags))
        fg::SaveSettings();
    ImGui::TextUnformatted("Log: QuarryDLSS.log next to the game executable.");
}

// Frame generation (fg.cpp), next to the DLSS choices: it needs DLSS Super Resolution running.
void DrawFrameGeneration() {
    auto& o = fg::g_options;
    const char* modes[static_cast<int>(fg::Mode::count)];
    for (int i = 0; i < static_cast<int>(fg::Mode::count); ++i) modes[i] = fg::ModeName(static_cast<fg::Mode>(i));
    int mode = static_cast<int>(o.mode);
    ImGui::BeginDisabled(!fg::Selectable() || !dlss::g_options.enabled);
    const bool changed = ImGui::Combo("Frame generation", &mode, modes, static_cast<int>(fg::Mode::count));
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("DLSS Frame Generation. 2x, 3x, 4x: each rendered frame plus 1-3 generated ones.\n"
                          "Dynamic: picks the multiplier that reaches your refresh rate.");
    if (!dlss::g_options.enabled && o.mode != fg::Mode::off)
        ImGui::TextWrapped("Paused: needs DLSS Super Resolution on.");
    else if (const std::string st = fg::FriendlyStatus(); !st.empty())
        ImGui::TextWrapped("%s", st.c_str());
    if (changed) {
        o.mode = static_cast<fg::Mode>(mode);
        Log("options at frame %llu: frame generation %s", static_cast<unsigned long long>(g_frame), fg::ModeName(o.mode));
        fg::SaveSettings();
    }
}

void DrawOverlay(effect_runtime*) {
    std::lock_guard lock(g_mutex);
    auto& o = dlss::g_options;
    ImGui::Checkbox("DLSS Super Resolution", &o.enabled);
    ImGui::TextWrapped("%s", FriendlyStatus().c_str());

    const char* names[kCustomQuality + 1];
    for (int i = 0; i <= kCustomQuality; ++i) names[i] = kQualityModes[i].name;
    if (ImGui::Combo("Quality mode", &g_quality, names, kCustomQuality + 1) && g_quality != kCustomQuality)
        g_forcePercentage = kQualityModes[g_quality].percentage;
    ImGui::Spacing();
    DrawFrameGeneration();

    if (ImGui::CollapsingHeader("Advanced")) {
        static const char* presetNames[] = {"J (recommended)", "K", "L", "M", "E (older CNN model)", "Driver default"};
        static const uint32_t presetValues[] = {10, 11, 12, 13, 5, 0};
        int current = 0;
        for (int i = 0; i < 6; ++i)
            if (presetValues[i] == o.preset) current = i;
        if (ImGui::Combo("DLSS model", &current, presetNames, 6)) o.preset = presetValues[current];
        ImGui::TextWrapped("An NVIDIA App DLSS model override for The Quarry replaces this. "
                           "K can smear some close-ups; J is recommended.");
        if (ImGui::SliderFloat("Custom render scale", &g_forcePercentage, 33.0f, 100.0f, "%.0f%%"))
            g_quality = kCustomQuality;
        ImGui::Checkbox("Stabilise screen-space lighting (SSGI/SSR mods)", &o.clampLightingHistory);
    }
    if (ImGui::CollapsingHeader("Diagnostics")) DrawDiagnostics();

    // Save settings, and record option changes so a test run can be read back from the log.
    static dlss::Options logged = o;
    static int loggedQuality = g_quality;
    static float loggedScale = g_forcePercentage;
    if (o.enabled != logged.enabled || o.flipJitterX != logged.flipJitterX || o.flipJitterY != logged.flipJitterY ||
        o.mvJittered != logged.mvJittered || o.invertMotion != logged.invertMotion ||
        o.ignoreVelocity != logged.ignoreVelocity || o.clampLightingHistory != logged.clampLightingHistory ||
        o.passPreExposure != logged.passPreExposure || o.gameExposure != logged.gameExposure || o.preset != logged.preset ||
        o.logBrightness != logged.logBrightness || o.verboseLog != logged.verboseLog || g_quality != loggedQuality ||
        g_forcePercentage != loggedScale) {
        Log("options at frame %llu: DLSS %s, %s %.1f%%, preset %u, stabilise lighting %d, pre-exposure %d, game exposure %d, "
            "flip jitter X %d Y %d, MVs jittered %d, invert MVs %d, ignore velocity %d, verbose log %d, log brightness %d",
            static_cast<unsigned long long>(g_frame), o.enabled ? "on" : "off", kQualityModes[g_quality].name,
            g_forcePercentage, o.preset, o.clampLightingHistory, o.passPreExposure, o.gameExposure, o.flipJitterX,
            o.flipJitterY, o.mvJittered, o.invertMotion, o.ignoreVelocity, o.verboseLog, o.logBrightness);
        logged = o;
        loggedQuality = g_quality;
        loggedScale = g_forcePercentage;
        SaveSettings();
    }
}

void Install() {
    OpenLog();
    LogGameInfo();
    dlss::g_log = &Log;
    fg::g_log = &Log;
    reshade::register_event<reshade::addon_event::init_device>(&OnInitDevice);
    reshade::register_event<reshade::addon_event::init_swapchain>(&OnInitSwapchain);
    reshade::register_event<reshade::addon_event::init_pipeline>(&OnInitPipeline);
    reshade::register_event<reshade::addon_event::destroy_pipeline>(&OnDestroyPipeline);
    reshade::register_event<reshade::addon_event::destroy_resource>(&OnDestroyResource);
    reshade::register_event<reshade::addon_event::init_resource>(&OnInitResource);
    reshade::register_event<reshade::addon_event::init_pipeline_layout>(&OnInitPipelineLayout);
    reshade::register_event<reshade::addon_event::update_descriptor_tables>(&OnUpdateDescriptorTables);
    reshade::register_event<reshade::addon_event::copy_descriptor_tables>(&OnCopyDescriptorTables);
    reshade::register_event<reshade::addon_event::bind_descriptor_tables>(&OnBindDescriptorTables);
    reshade::register_event<reshade::addon_event::reset_command_list>(&OnResetCommandList);
    reshade::register_event<reshade::addon_event::push_constants>(&OnPushConstants);
    reshade::register_event<reshade::addon_event::destroy_device>(&OnDestroyDevice);
    reshade::register_event<reshade::addon_event::map_buffer_region>(&OnMapBuffer);
    reshade::register_event<reshade::addon_event::unmap_buffer_region>(&OnUnmapBuffer);
    reshade::register_event<reshade::addon_event::update_buffer_region>(&OnUpdateBuffer);
    reshade::register_event<reshade::addon_event::update_buffer_region_command>(&OnUpdateBufferCommand);
    reshade::register_event<reshade::addon_event::bind_pipeline>(&OnBindPipeline);
    reshade::register_event<reshade::addon_event::push_descriptors>(&OnPushDescriptors);
    reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(&OnBindTargets);
    reshade::register_event<reshade::addon_event::bind_viewports>(&OnBindViewports);
    reshade::register_event<reshade::addon_event::draw>(&OnDraw);
    reshade::register_event<reshade::addon_event::draw_indexed>(&OnDrawIndexed);
    reshade::register_event<reshade::addon_event::dispatch>(&OnDispatch);
    reshade::register_event<reshade::addon_event::copy_resource>(&OnCopyResource);
    reshade::register_event<reshade::addon_event::present>(&OnPresent);
    reshade::register_event<reshade::addon_event::reshade_present>(&OnReshadePresent);
    LoadSettings();
    fg::LoadSettings();
    reshade::register_overlay("QuarryDLSS", &DrawOverlay);
}

void Uninstall() {
    reshade::unregister_overlay("QuarryDLSS", &DrawOverlay);
    fg::Shutdown();
    if (g_log) {
        Log("unloading");
        fclose(g_log);
        g_log = nullptr;
    }
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            if (!reshade::register_addon(module)) return FALSE;
            Install();
            break;
        case DLL_PROCESS_DETACH:
            Uninstall();
            reshade::unregister_addon(module);  // also removes the event callbacks
            break;
    }
    return TRUE;
}
