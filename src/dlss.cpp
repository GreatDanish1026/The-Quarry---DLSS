// DLSS Super Resolution in place of The Quarry's TAAU dispatch (Direct3D 12).
//
// Per frame, on the game's command list, at the moment the game would dispatch its
// TAAU compute shader (inputs already in SRV state, output in UAV state):
//   1. dlss_prepare.hlsl (through ReShade) writes DLSS motion vectors and R32F depth
//      for the render view rect into two textures of ours;
//   2. NGX evaluates DLSS straight into the TAAU's own output texture.
// The caller then restores the game's compute state and skips the game's dispatch.
// NGX binds its own heaps, root signature and pipeline natively, so the caller has
// to re-bind the game's state through ReShade (see RestoreComputeState in probe.cpp).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>

#include "dlss.hpp"
#include "fg.hpp"

#include <nvsdk_ngx_helpers_d3d.h>

#include <cstdio>
#include <algorithm>
#include <unordered_map>
#include <vector>

#include "dlss_clamp_cs.h"    // kDlssClampCS, from shaders/dlss_clamp.hlsl
#include "dlss_prepare_cs.h"  // kDlssPrepareCS, compiled by build.ps1 from shaders/dlss_prepare.hlsl

#pragma comment(lib, "nvsdk_ngx_s.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "user32.lib")

namespace dlss {

Options g_options;
void (*g_log)(const char* fmt, ...) = nullptr;

namespace {

#define LOG(...)                       \
    do {                               \
        if (g_log) g_log(__VA_ARGS__); \
    } while (0)

// NGX project id for this mod (any fixed GUID; identifies the app to NGX).
constexpr char kProjectId[] = "6c2f3b1e-9d4a-4e57-8b6f-2a1d7c9e0f53";
constexpr uint32_t kConstants = 24;  // float4x4 + float2 + float2 + float4 flags

enum class Init { untried, ok, failed };
Init g_init = Init::untried;
device* g_device = nullptr;
ID3D12Device* g_d3d = nullptr;
NVSDK_NGX_Parameter* g_params = nullptr;

// DLSS features are only ever created at present time, on ReShade's immediate
// command list (Tick). A TAAU dispatch with no feature for its sizes records nothing:
// it asks for one (g_wanted) and the game's TAAU runs that frame.
//
// The game switches between letterboxed (2576x1072 -> 3840x1600) and full-frame
// (2576x1448 -> 3840x2160) shots, with different output textures. One feature can't
// serve both (DLSS output size is fixed at creation; output subrects only offset it),
// so features are cached per size and kept, and a shot change just switches feature.
struct FeatureKey {
    uint32_t renderW = 0, renderH = 0, outW = 0, outH = 0;
    bool jittered = false;
    bool autoExposure = true;  // DLSS's own exposure; false = the game's eye adaptation texture
    uint32_t preset = 0;       // NVSDK_NGX_DLSS_Hint_Render_Preset_*, 0 = default (a driver override wins)
    bool operator==(const FeatureKey& o) const {
        return renderW == o.renderW && renderH == o.renderH && outW == o.outW && outH == o.outH &&
               jittered == o.jittered && autoExposure == o.autoExposure &&
               preset == o.preset;
    }
    bool operator!=(const FeatureKey& o) const { return !(*this == o); }
};
struct Feature {
    FeatureKey key;
    NVSDK_NGX_Handle* handle = nullptr;
    uint64_t lastUsed = 0;  // evaluate count when last used
};
constexpr size_t kMaxFeatures = 4;
std::vector<Feature> g_features;
const Feature* g_lastFeature = nullptr;  // used by the previous evaluate (history continuity)
FeatureKey g_wanted;                      // what the last TAAU dispatch needed
FeatureKey g_failedKey;                   // last creation that failed (not retried)

Feature* FindFeature(const FeatureKey& key) {
    for (auto& f : g_features)
        if (f.key == key) return &f;
    return nullptr;
}

pipeline_layout g_layout = {};
pipeline g_pso = {};

resource g_motion = {}, g_depth = {};
resource_view g_motionUav = {}, g_depthUav = {};
uint32_t g_texW = 0, g_texH = 0;

std::unordered_map<uint64_t, resource_view> g_srvs;  // game texture -> our SRV of it
std::unordered_map<uint64_t, resource_view> g_uavs;  // game texture -> our UAV of it

// Clamped copy of the DLSS output (g_hist), made right after DLSS in frame N and copied
// over that same output texture (g_histOutput) just before the first pass of frame N+1
// reads it, i.e. the game's SSGI/SSR. See dlss_clamp.hlsl.
pipeline_layout g_clampLayout = {};
pipeline g_clampPso = {};
resource g_hist = {};
resource_view g_histUav = {};
uint32_t g_histW = 0, g_histH = 0;
resource g_histOutput = {};    // the game texture DLSS wrote and g_hist mirrors
uint64_t g_histFrame = 0;      // frame g_hist was made in
uint64_t g_histPatchedFrame = 0;
uint64_t g_histPatches = 0;

// Swap chain resizes (F11) hung the GPU with DLSS on (FINDINGS.md, runs 6-8): for
// kTraceFrames DLSS frames after each resize, every evaluate and lighting copy is logged.
constexpr uint32_t kTraceFrames = 30;
uint32_t g_traceFrames = 0;
// The lighting history copy assumes one DLSS frame per present, with last frame's output
// texture untouched until the lighting reads it. After a swap chain resize the game renders
// a frame twice (77% with DLSS, then 100% with its own TAA) before one present, and with the
// copy on that hung the GPU (FINDINGS.md, run 9). So the copy is cancelled when a TAAU
// dispatch falls back after DLSS ran, and held off for kHistHoldFrames DLSS frames after a resize.
constexpr uint32_t kHistHoldFrames = 30;
uint32_t g_histHoldFrames = 0;

std::string g_status = "waiting for the game's TAAU pass";
uint64_t g_done = 0, g_failures = 0;
constexpr uint32_t kCameraOnlyFrames = 2;  // after a reset, see Evaluate
constexpr uint32_t kStaleWindow = 90;
constexpr float kStaleThreshold = 24.0f;  // render pixels
constexpr uint32_t kResetLogFrames = 40;  // per-frame diagnostics after a reset
uint32_t g_resetLogLines = 0;
uint32_t g_framesSinceReset = 1000;
uint64_t g_lastEvalFrame = 0;  // frame of the last successful evaluate

void SetStatus(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_status != buf) LOG("dlss: %s", buf);
    g_status = buf;
}

// NGX's own log lines, into QuarryDLSS.log (may be called from NGX's threads).
void NVSDK_CONV OnNgxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    std::string m = message ? message : "";
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
    LOG("ngx: %s", m.c_str());
}

bool InitNgx(device* dev) {
    g_device = dev;
    g_d3d = reinterpret_cast<ID3D12Device*>(dev->get_native());
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path, n);
    dir.resize(dir.find_last_of(L"\\/") + 1);

    NVSDK_NGX_FeatureCommonInfo info = {};
    info.LoggingInfo.LoggingCallback = &OnNgxLog;
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    info.LoggingInfo.DisableOtherLoggingSinks = true;
    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_UNREAL, "4.26",
                                                             dir.c_str(), g_d3d, &info);
    if (NVSDK_NGX_FAILED(r)) {
        SetStatus("NGX init failed (0x%08x); is nvngx_dlss.dll next to the game exe?", static_cast<unsigned>(r));
        return false;
    }
    r = NVSDK_NGX_D3D12_GetCapabilityParameters(&g_params);
    if (NVSDK_NGX_FAILED(r) || !g_params) {
        SetStatus("NGX capability query failed (0x%08x)", static_cast<unsigned>(r));
        return false;
    }
    int available = 0, needsDriver = 0;
    NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
    if (!available) {
        SetStatus("DLSS Super Resolution not available%s", needsDriver ? " (driver update needed)" : "");
        return false;
    }
    LOG("dlss: NGX initialised, DLSS Super Resolution available");
    return true;
}

// ---- motion vector diagnostics: the prepare pass counts into a small GPU buffer
// (see Stats in dlss_prepare.hlsl), which is copied to a readback buffer and read
// kStatsRing frames later, then logged every kStatsLogEvery DLSS frames.

constexpr uint32_t kStatsRing = 4;
constexpr uint32_t kStatsWords = 12;
constexpr uint64_t kStatsLogEvery = 60;
resource g_stats[kStatsRing] = {}, g_statsReadback[kStatsRing] = {}, g_statsZero = {};
resource_view g_statsUav[kStatsRing] = {};
bool g_statsPending[kStatsRing] = {};
uint32_t g_statsAge[kStatsRing] = {};  // frames since the last reset, per slot
uint64_t g_statsFrame = 0;
uint64_t g_statsSum[kStatsWords] = {};  // totals since the last log line
uint32_t g_statsMax = 0, g_statsDepthMin = 0xffffffffu, g_statsDepthMax = 0, g_statsFrames = 0;

bool CreateStats(device* dev) {
    const uint32_t zeros[kStatsWords] = {};
    const subresource_data init = {const_cast<uint32_t*>(zeros), sizeof(zeros), sizeof(zeros)};
    if (!dev->create_resource(resource_desc(sizeof(zeros), memory_heap::default_, resource_usage::copy_source), &init,
                              resource_usage::copy_source, &g_statsZero))
        return false;
    for (uint32_t i = 0; i < kStatsRing; ++i) {
        const resource_desc gpu(sizeof(zeros), memory_heap::default_,
                                resource_usage::unordered_access | resource_usage::copy_source | resource_usage::copy_dest);
        const resource_desc cpu(sizeof(zeros), memory_heap::readback, resource_usage::copy_dest);
        if (!dev->create_resource(gpu, nullptr, resource_usage::unordered_access, &g_stats[i]) ||
            // Typed buffer views: ReShade passes offset/size through as FirstElement/NumElements
            // (elements, not bytes). A byte count here made an out-of-bounds UAV: device removed.
            !dev->create_resource_view(g_stats[i], resource_usage::unordered_access,
                                       resource_view_desc(format::r32_uint, 0, kStatsWords), &g_statsUav[i]) ||
            !dev->create_resource(cpu, nullptr, resource_usage::copy_dest, &g_statsReadback[i]))
            return false;
    }
    return true;
}

void DestroyStats(device* dev) {
    for (uint32_t i = 0; i < kStatsRing; ++i) {
        if (g_statsUav[i].handle) dev->destroy_resource_view(g_statsUav[i]);
        if (g_stats[i].handle) dev->destroy_resource(g_stats[i]);
        if (g_statsReadback[i].handle) dev->destroy_resource(g_statsReadback[i]);
        g_statsUav[i] = {};
        g_stats[i] = g_statsReadback[i] = {};
        g_statsPending[i] = false;
    }
    if (g_statsZero.handle) dev->destroy_resource(g_statsZero);
    g_statsZero = {};
}

// Reads the slot written kStatsRing frames ago (long done on the GPU) and logs a summary.
void CollectStats(device* dev, uint32_t slot) {
    if (!g_statsPending[slot]) return;
    void* p = nullptr;
    if (!dev->map_buffer_region(g_statsReadback[slot], 0, kStatsWords * 4, map_access::read_only, &p) || !p) return;
    uint32_t s[kStatsWords];
    memcpy(s, p, sizeof(s));
    dev->unmap_buffer_region(g_statsReadback[slot]);
    g_statsPending[slot] = false;
    // Frame by frame right after a reset: how long does the game's velocity stay wrong?
    if (g_options.verboseLog && g_statsAge[slot] < kResetLogFrames && s[1] && ++g_resetLogLines <= 600)
        LOG("dlss: reset+%u: object pixels %.0f%%, decoded %.2f px vs camera %.2f px, %.1f%% inflated, "
            "%.1f%% replaced by camera motion, max motion %.1f px",
            g_statsAge[slot], 100.0 * s[1] / (s[0] ? s[0] : 1), s[8] / 16.0 / s[1], s[9] / 16.0 / s[1],
            100.0 * s[10] / s[1], 100.0 * s[11] / s[1], s[4] / 16.0);
    for (uint32_t i = 0; i < 4; ++i) g_statsSum[i] += s[i];
    g_statsSum[5] += s[5];
    for (uint32_t i = 8; i <= 10; ++i) g_statsSum[i] += s[i];
    g_statsMax = std::max(g_statsMax, s[4]);
    if (s[6]) g_statsDepthMin = std::min(g_statsDepthMin, ~s[6]);
    g_statsDepthMax = std::max(g_statsDepthMax, s[7]);
    if (++g_statsFrames < kStatsLogEvery) return;
    if (!g_options.verboseLog) {
        memset(g_statsSum, 0, sizeof(g_statsSum));
        g_statsMax = g_statsDepthMax = g_statsFrames = 0;
        g_statsDepthMin = 0xffffffffu;
        return;
    }

    const double n = g_statsSum[0] ? double(g_statsSum[0]) : 1.0;
    float dmin = 0, dmax = 0;
    memcpy(&dmin, &g_statsDepthMin, 4);
    memcpy(&dmax, &g_statsDepthMax, 4);
    LOG("dlss: motion over %u frames: %.1f%% object velocity, %.1f%% moving, mean %.2f px, max %.1f px, "
        "%.1f%% sky, depth %.6f..%.6f",
        g_statsFrames, 100.0 * g_statsSum[1] / n, 100.0 * g_statsSum[2] / n, g_statsSum[3] / 4.0 / n, g_statsMax / 16.0,
        100.0 * g_statsSum[5] / n, g_statsDepthMin == 0xffffffffu ? 0.0f : dmin, dmax);
    // Object-velocity pixels: decoded game velocity vs what camera motion alone predicts.
    // On static objects these match if the decode is right.
    if (g_statsSum[1]) {
        const double m = double(g_statsSum[1]);
        LOG("dlss: object-velocity pixels: decoded mean %.2f px vs camera-only mean %.2f px; "
            "%.1f%% decoded more than 4x camera + 1 px%s",
            g_statsSum[8] / 16.0 / m, g_statsSum[9] / 16.0 / m, 100.0 * g_statsSum[10] / m,
            g_options.ignoreVelocity ? " (game velocity ignored)" : "");
    }
    memset(g_statsSum, 0, sizeof(g_statsSum));
    g_statsMax = g_statsDepthMax = g_statsFrames = 0;
    g_statsDepthMin = 0xffffffffu;
}

// ---- brightness diagnostics: the clamp pass measures the luma of the input colour, the
// DLSS output and the clamped copy (see Stats in dlss_clamp.hlsl) into a ring like the one
// above, logged one line per frame to show where a blow-up starts.

resource g_luma[kStatsRing] = {}, g_lumaReadback[kStatsRing] = {};
resource_view g_lumaUav[kStatsRing] = {};
bool g_lumaPending[kStatsRing] = {};
uint64_t g_lumaFrame[kStatsRing] = {};  // probe frame, per slot
uint32_t g_lumaAge[kStatsRing] = {};    // frames since the last reset, per slot
bool g_lumaClamp[kStatsRing] = {};      // clamped copy handed to the lighting that frame
float g_lumaPreExposure[kStatsRing] = {};  // View.PreExposure that frame
uint64_t g_patchedRing[8] = {};         // frames whose lighting history was patched (frame % 8)

bool CreateLuma(device* dev) {
    for (uint32_t i = 0; i < kStatsRing; ++i) {
        const resource_desc gpu(kStatsWords * 4, memory_heap::default_,
                                resource_usage::unordered_access | resource_usage::copy_source | resource_usage::copy_dest);
        const resource_desc cpu(kStatsWords * 4, memory_heap::readback, resource_usage::copy_dest);
        if (!dev->create_resource(gpu, nullptr, resource_usage::unordered_access, &g_luma[i]) ||
            !dev->create_resource_view(g_luma[i], resource_usage::unordered_access,
                                       resource_view_desc(format::r32_uint, 0, kStatsWords), &g_lumaUav[i]) ||
            !dev->create_resource(cpu, nullptr, resource_usage::copy_dest, &g_lumaReadback[i]))
            return false;
    }
    return true;
}

void DestroyLuma(device* dev) {
    for (uint32_t i = 0; i < kStatsRing; ++i) {
        if (g_lumaUav[i].handle) dev->destroy_resource_view(g_lumaUav[i]);
        if (g_luma[i].handle) dev->destroy_resource(g_luma[i]);
        if (g_lumaReadback[i].handle) dev->destroy_resource(g_lumaReadback[i]);
        g_lumaUav[i] = {};
        g_luma[i] = g_lumaReadback[i] = {};
        g_lumaPending[i] = false;
    }
}

void CollectLuma(device* dev, uint32_t slot) {
    if (!g_lumaPending[slot]) return;
    void* p = nullptr;
    if (!dev->map_buffer_region(g_lumaReadback[slot], 0, kStatsWords * 4, map_access::read_only, &p) || !p) return;
    uint32_t s[kStatsWords];
    memcpy(s, p, sizeof(s));
    dev->unmap_buffer_region(g_lumaReadback[slot]);
    g_lumaPending[slot] = false;
    if (!g_options.logBrightness || !s[0]) return;
    float maxima[3];
    memcpy(maxima, &s[4], sizeof(maxima));
    const double n = double(s[0]) * 64.0;  // kSumScale in dlss_clamp.hlsl
    const uint64_t f = g_lumaFrame[slot];
    // "lit" says whether the lighting read the clamped copy in the next frame.
    const char* lit = !g_lumaClamp[slot] ? "off" : g_patchedRing[(f + 1) % 8] == f + 1 ? "yes" : "no";
    LOG("luma f%llu age %u pre %.4g: in %.3f max %.1f | dlss %.3f max %.1f | clamped %.3f max %.1f | cut %.1f%% "
        "hot %.2f%%/%.2f%% (dlss/in) nonfinite %u/%u (dlss/in) lit %s",
        static_cast<unsigned long long>(f), g_lumaAge[slot], g_lumaPreExposure[slot], s[1] / n, maxima[0], s[2] / n, maxima[1], s[3] / n,
        maxima[2], 100.0 * s[7] / s[0], 100.0 * s[10] / s[0], 100.0 * s[11] / s[0], s[9], s[8], lit);
}

// ---- texture capture (RequestCapture): one DLSS frame's inputs, raw, for offline analysis.
// Copied to readback buffers in the game's command list, written to files a few presents
// later (long done on the GPU): QuarryDLSS_capture_<frame>_<name>.bin plus a .txt header.

struct CaptureItem {
    std::string name;
    ID3D12Resource* buffer = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT rows = 0;
    UINT64 rowBytes = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};
bool g_captureWanted = false;
bool g_captureSecond = false;  // the first of the pair is copied; take the next frame
bool g_captureArmed = false;     // capture kCaptureArmedAge frames after the next reset
bool g_captureSawReset = false;
constexpr uint32_t kCaptureArmedAge = 45;
uint32_t g_captureWait = 0;  // presents until the copies are read back
std::vector<CaptureItem> g_capture;
std::string g_captureHeader;
uint64_t g_captureFrame = 0;
std::string g_captureStatus;

std::string GameDir() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string dir = path;
    return dir.substr(0, dir.find_last_of("\\/") + 1);
}

// Records a copy of subresource 0 of <res> (currently in <state>) into a new readback buffer.
void CaptureTexture(command_list* cmd, resource res, resource_usage state, const char* name) {
    auto* tex = reinterpret_cast<ID3D12Resource*>(res.handle);
    const D3D12_RESOURCE_DESC desc = tex->GetDesc();
    CaptureItem item;
    item.name = name;
    item.format = desc.Format;
    UINT64 total = 0;
    g_d3d->GetCopyableFootprints(&desc, 0, 1, 0, &item.footprint, &item.rows, &item.rowBytes, &total);
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buf = {};
    buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf.Width = total;
    buf.Height = 1;
    buf.DepthOrArraySize = 1;
    buf.MipLevels = 1;
    buf.SampleDesc.Count = 1;
    buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(g_d3d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&item.buffer)))) {
        LOG("capture: could not create a %llu-byte readback buffer for %s", static_cast<unsigned long long>(total), name);
        return;
    }
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = item.buffer;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = item.footprint;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = tex;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    cmd->barrier(res, state, resource_usage::copy_source);
    reinterpret_cast<ID3D12GraphicsCommandList*>(cmd->get_native())->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    cmd->barrier(res, resource_usage::copy_source, state);
    g_capture.push_back(item);
}

void ReleaseCapture() {
    g_captureSecond = false;
    g_captureWait = 0;
    for (auto& item : g_capture)
        if (item.buffer) item.buffer->Release();
    g_capture.clear();
}

// At present: once the copies are surely done, writes them out.
void FinishCapture() {
    if (g_capture.empty() || g_captureWait == 0 || --g_captureWait > 0) return;
    const std::string base = GameDir() + "QuarryDLSS_capture_" + std::to_string(g_captureFrame);
    std::string header = g_captureHeader;
    for (auto& item : g_capture) {
        void* p = nullptr;
        // Whole buffer: the last row isn't padded to RowPitch, so rows * RowPitch overruns it.
        const HRESULT hr = item.buffer->Map(0, nullptr, &p);
        if (FAILED(hr) || !p) {
            LOG("capture: could not map the %s readback buffer (0x%08x)", item.name.c_str(), static_cast<unsigned>(hr));
            continue;
        }
        const std::string file = base + "_" + item.name + ".bin";
        if (FILE* f = fopen(file.c_str(), "wb")) {
            for (UINT y = 0; y < item.rows; ++y)
                fwrite(static_cast<const uint8_t*>(p) + item.footprint.Offset + size_t(y) * item.footprint.Footprint.RowPitch,
                       1, static_cast<size_t>(item.rowBytes), f);
            fclose(f);
        }
        const D3D12_RANGE none = {0, 0};
        item.buffer->Unmap(0, &none);
        char line[200];
        snprintf(line, sizeof(line), "texture %s %ux%u dxgi_format %d bytes_per_row %llu file %s\n", item.name.c_str(),
                 item.footprint.Footprint.Width, item.rows, static_cast<int>(item.format),
                 static_cast<unsigned long long>(item.rowBytes), (base + "_" + item.name + ".bin").c_str());
        header += line;
    }
    if (FILE* f = fopen((base + ".txt").c_str(), "w")) {
        fputs(header.c_str(), f);
        fclose(f);
    }
    LOG("capture: wrote %s.txt and %zu textures", base.c_str(), g_capture.size());
    g_captureStatus = "wrote " + base + ".txt";
    ReleaseCapture();
}

bool CreatePipeline(device* dev) {
    pipeline_layout_param params[3];
    params[0] = pipeline_layout_param(constant_range{0, 0, 0, kConstants, shader_stage::compute});

    descriptor_range srvs;
    srvs.binding = 0;
    srvs.dx_register_index = 0;
    srvs.count = 2;
    srvs.visibility = shader_stage::compute;
    srvs.type = descriptor_type::shader_resource_view;
    descriptor_range uavs = srvs;
    uavs.count = 3;  // motion, depth, stats buffer
    uavs.type = descriptor_type::unordered_access_view;
    params[1].type = pipeline_layout_param_type::push_descriptors_with_ranges;
    params[1].descriptor_table = {1, &srvs};
    params[2].type = pipeline_layout_param_type::push_descriptors_with_ranges;
    params[2].descriptor_table = {1, &uavs};

    if (!dev->create_pipeline_layout(3, params, &g_layout)) {
        SetStatus("could not create the prepare pass root signature");
        return false;
    }
    shader_desc cs;
    cs.code = kDlssPrepareCS;
    cs.code_size = sizeof(kDlssPrepareCS);
    pipeline_subobject sub = {pipeline_subobject_type::compute_shader, 1, &cs};
    if (!dev->create_pipeline(g_layout, 1, &sub, &g_pso)) {
        SetStatus("could not create the prepare pass pipeline");
        return false;
    }

    // Clamp pass (dlss_clamp.hlsl): b0 constants, t0 input colour, u0 DLSS output, u1 ours,
    // u2 brightness counters.
    pipeline_layout_param clampParams[3];
    clampParams[0] = pipeline_layout_param(constant_range{0, 0, 0, 4, shader_stage::compute});
    descriptor_range clampSrv = srvs;
    clampSrv.count = 1;
    descriptor_range clampUav = uavs;
    clampUav.count = 3;
    clampParams[1].type = pipeline_layout_param_type::push_descriptors_with_ranges;
    clampParams[1].descriptor_table = {1, &clampSrv};
    clampParams[2].type = pipeline_layout_param_type::push_descriptors_with_ranges;
    clampParams[2].descriptor_table = {1, &clampUav};
    shader_desc clampCs;
    clampCs.code = kDlssClampCS;
    clampCs.code_size = sizeof(kDlssClampCS);
    pipeline_subobject clampSub = {pipeline_subobject_type::compute_shader, 1, &clampCs};
    if (!dev->create_pipeline_layout(3, clampParams, &g_clampLayout) ||
        !dev->create_pipeline(g_clampLayout, 1, &clampSub, &g_clampPso)) {
        SetStatus("could not create the history clamp pipeline");
        return false;
    }
    return CreateStats(dev) && CreateLuma(dev);
}

// ---- clamped lighting history (see dlss_clamp.hlsl) --------------------------------

void DestroyHistory(device* dev) {
    if (g_histUav.handle) dev->destroy_resource_view(g_histUav);
    if (g_hist.handle) dev->destroy_resource(g_hist);
    g_histUav = {};
    g_hist = {};
    g_histW = g_histH = 0;
    g_histOutput = {};
}

bool EnsureHistory(device* dev, uint32_t w, uint32_t h) {
    if (g_hist.handle && g_histW == w && g_histH == h) return true;
    DestroyHistory(dev);
    const resource_desc desc(w, h, 1, 1, format::r16g16b16a16_float, 1, memory_heap::default_,
                             resource_usage::shader_resource | resource_usage::unordered_access |
                                 resource_usage::copy_source);
    if (!dev->create_resource(desc, nullptr, resource_usage::unordered_access, &g_hist) ||
        !dev->create_resource_view(g_hist, resource_usage::unordered_access,
                                   resource_view_desc(format::r16g16b16a16_float), &g_histUav)) {
        DestroyHistory(dev);
        return false;
    }
    g_histW = w;
    g_histH = h;
    LOG("dlss: clamped lighting history texture %ux%u", w, h);
    return true;
}

// D3D12 removes the device (DXGI_ERROR_INVALID_CALL) for a view in a format the texture can't
// take. The Devil in Me's scene colour is R11G11B10F where The Quarry and House of Ashes use
// RGBA16F, and an RGBA16F view of it killed the game on its first DLSS frame. So views of the
// game's textures use the texture's own format (typeless ones get the typed format the shader
// reads), and a format a shader can't decode is refused: format::unknown.
format VelocityViewFormat(format f) {  // dlss_prepare.hlsl decodes UE's 16-bit UNORM velocity
    switch (f) {
        case format::r16g16b16a16_unorm:
        case format::r16g16b16a16_typeless: return format::r16g16b16a16_unorm;
        case format::r16g16_unorm:
        case format::r16g16_typeless: return format::r16g16_unorm;
        default: return format::unknown;
    }
}

format DepthViewFormat(format f) {  // the depth value in .r
    switch (f) {
        case format::r32_g8_typeless:
        case format::d32_float_s8_uint:
        case format::r32_float_x8_uint: return format::r32_float_x8_uint;
        case format::r32_typeless:
        case format::d32_float:
        case format::r32_float: return format::r32_float;
        case format::r24_g8_typeless:
        case format::d24_unorm_s8_uint:
        case format::r24_unorm_x8_uint: return format::r24_unorm_x8_uint;
        case format::r16_typeless:
        case format::d16_unorm:
        case format::r16_unorm: return format::r16_unorm;
        default: return format::unknown;
    }
}

format ColorViewFormat(format f) {  // dlss_clamp.hlsl reads float RGB
    switch (f) {
        case format::r16g16b16a16_float:
        case format::r16g16b16a16_typeless: return format::r16g16b16a16_float;
        case format::r11g11b10_float: return format::r11g11b10_float;
        case format::r32g32b32a32_float:
        case format::r32g32b32a32_typeless: return format::r32g32b32a32_float;
        case format::r10g10b10a2_unorm:
        case format::r10g10b10a2_typeless: return format::r10g10b10a2_unorm;
        default: return format::unknown;
    }
}

resource_view GameUav(device* dev, resource res, format fmt) {
    auto it = g_uavs.find(res.handle);
    if (it != g_uavs.end()) return it->second;
    resource_view view = {};
    if (!dev->create_resource_view(res, resource_usage::unordered_access, resource_view_desc(fmt), &view)) return {};
    g_uavs[res.handle] = view;
    return view;
}

void DestroyTextures(device* dev) {
    if (g_motionUav.handle) dev->destroy_resource_view(g_motionUav);
    if (g_depthUav.handle) dev->destroy_resource_view(g_depthUav);
    if (g_motion.handle) dev->destroy_resource(g_motion);
    if (g_depth.handle) dev->destroy_resource(g_depth);
    g_motionUav = g_depthUav = {};
    g_motion = g_depth = {};
    g_texW = g_texH = 0;
}

bool CreateTexture(device* dev, uint32_t w, uint32_t h, format fmt, resource& res, resource_view& uav) {
    const resource_desc desc(w, h, 1, 1, fmt, 1, memory_heap::default_,
                             resource_usage::shader_resource | resource_usage::unordered_access);
    return dev->create_resource(desc, nullptr, resource_usage::unordered_access, &res) &&
           dev->create_resource_view(res, resource_usage::unordered_access, resource_view_desc(fmt), &uav);
}

bool EnsureTextures(device* dev, uint32_t w, uint32_t h) {
    if (g_motion.handle && g_texW == w && g_texH == h) return true;
    DestroyTextures(dev);
    if (!CreateTexture(dev, w, h, format::r16g16_float, g_motion, g_motionUav) ||
        !CreateTexture(dev, w, h, format::r32_float, g_depth, g_depthUav)) {
        DestroyTextures(dev);
        SetStatus("could not create the %ux%u motion vector / depth textures", w, h);
        return false;
    }
    g_texW = w;
    g_texH = h;
    LOG("dlss: motion vector and depth textures %ux%u", w, h);
    return true;
}

resource_view GameSrv(device* dev, resource res, format fmt) {
    auto it = g_srvs.find(res.handle);
    if (it != g_srvs.end()) return it->second;
    resource_view view = {};
    if (!dev->create_resource_view(res, resource_usage::shader_resource, resource_view_desc(fmt), &view)) return {};
    g_srvs[res.handle] = view;
    return view;
}

// DLSS takes any render size; the mode only picks its tuning. NVSDK_NGX_PerfQuality_Value_UltraQuality
// is not accepted (feature creation fails with 0xbad00010, FAIL_UnsupportedParameter), so the 77%
// Ultra Quality mode is created as Quality.
NVSDK_NGX_PerfQuality_Value QualityFor(float fraction) {
    if (fraction > 0.9f) return NVSDK_NGX_PerfQuality_Value_DLAA;
    if (fraction > 0.62f) return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    if (fraction > 0.55f) return NVSDK_NGX_PerfQuality_Value_Balanced;
    if (fraction > 0.4f) return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
}


// Present time only, on ReShade's immediate command list for the game's queue, so a
// failure never leaves anything in the game's own command lists.
void CreateFeature(command_queue* queue, const FeatureKey& key) {
    if (g_features.size() >= kMaxFeatures) {
        // Evict the least recently used. It may still be in use by the GPU: wait (rare).
        auto lru = std::min_element(g_features.begin(), g_features.end(),
                                    [](const Feature& a, const Feature& b) { return a.lastUsed < b.lastUsed; });
        queue->wait_idle();
        NVSDK_NGX_D3D12_ReleaseFeature(lru->handle);
        LOG("dlss: released feature %ux%u -> %ux%u", lru->key.renderW, lru->key.renderH, lru->key.outW, lru->key.outH);
        g_features.erase(lru);
        g_lastFeature = nullptr;  // pointers into g_features moved
    }
    NVSDK_NGX_Handle* handle = nullptr;
    NVSDK_NGX_DLSS_Create_Params cp = {};
    cp.Feature.InWidth = key.renderW;
    cp.Feature.InHeight = key.renderH;
    cp.Feature.InTargetWidth = key.outW;
    cp.Feature.InTargetHeight = key.outH;
    cp.Feature.InPerfQualityValue = QualityFor(float(key.renderW) / float(key.outW));
    // Model preset hint for every quality mode. An NVIDIA App / driver (DRS) override takes precedence.
    for (const char* hint : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
                             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality})
        NVSDK_NGX_Parameter_SetUI(g_params, hint, key.preset);
    cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                              NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
                              (key.autoExposure ? NVSDK_NGX_DLSS_Feature_Flags_AutoExposure : 0) |
                              (key.jittered ? NVSDK_NGX_DLSS_Feature_Flags_MVJittered : 0);
    command_list* cmd = queue->get_immediate_command_list();
    auto list = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd->get_native());
    const NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSS_EXT(list, 1, 1, &handle, g_params, &cp);
    queue->flush_immediate_command_list();
    if (NVSDK_NGX_FAILED(r) || !handle) {
        g_failedKey = key;
        SetStatus("DLSS feature creation failed (0x%08x) for %ux%u -> %ux%u", static_cast<unsigned>(r), key.renderW,
                  key.renderH, key.outW, key.outH);
        return;
    }
    g_lastFeature = nullptr;  // push_back may move the others
    g_features.push_back({key, handle, g_done});
    LOG("dlss: feature created %ux%u -> %ux%u (quality mode %d%s, %s, preset hint %u), %zu cached", key.renderW, key.renderH, key.outW,
        key.outH, static_cast<int>(cp.Feature.InPerfQualityValue), key.jittered ? ", jittered MVs" : "",
        key.autoExposure ? "auto exposure" : "game exposure", key.preset, g_features.size());
    SetStatus("ready: %ux%u -> %ux%u", key.renderW, key.renderH, key.outW, key.outH);
}

}  // namespace

void Tick(effect_runtime* runtime) {
    device* dev = runtime->get_device();
    if (dev->get_api() != device_api::d3d12) return;
    // NGX init on the device that presents, at present time: never mid-frame.
    if (g_init == Init::untried) {
        g_init = InitNgx(dev) ? Init::ok : Init::failed;
        if (g_init == Init::ok && !CreatePipeline(dev)) g_init = Init::failed;
    }
    if (dev == g_device) FinishCapture();
    if (g_init != Init::ok || dev != g_device || !g_options.enabled) return;
    if (g_wanted.renderW && !FindFeature(g_wanted) && g_wanted != g_failedKey)
        CreateFeature(runtime->get_command_queue(), g_wanted);
}

Result Evaluate(const Frame& f) {
    if (!g_options.enabled || g_init != Init::ok || f.cmd->get_device() != g_device) return Result::skipped;
    device* dev = g_device;

    FeatureKey key;
    key.renderW = f.renderW;
    key.renderH = f.renderH;
    key.outW = f.outW;
    key.outH = f.outH;
    key.jittered = g_options.mvJittered;
    key.autoExposure = !(g_options.gameExposure && f.exposure.handle);
    key.preset = g_options.preset;
    Feature* feature = FindFeature(key);
    if (!feature) {
        g_wanted = key;  // created at the next present; the game's TAAU runs this frame
        return Result::skipped;
    }
    // A feature last used shots ago holds stale history: start it fresh.
    const bool switched = feature != g_lastFeature;

    // After a cut the game's velocity buffer is wrong for a while on many object pixels
    // (measured over the ~60 frames after the "And scene." cut: 47 px decoded vs 4.6 px
    // camera motion, up to 309 px), and DLSS reprojects its history with it. So: reset on
    // the cut frame and the next one, camera-only motion (ClipToPrevClip is right at a cut)
    // for kCameraOnlyFrames, then for kStaleWindow frames per pixel: game velocity that
    // disagrees with camera motion by more than kStaleThreshold px is replaced by it.
    // Any frame the game TAAU ran instead of DLSS leaves DLSS history a frame behind: reset.
    const bool gap = g_lastEvalFrame + 1 != f.frame;
    if (f.reset || switched || gap) g_framesSinceReset = 0;
    const uint32_t age = g_framesSinceReset;
    const bool reset = age <= 1;
    const bool cameraOnly = g_options.ignoreVelocity || age < kCameraOnlyFrames;
    const float staleThreshold = age < kStaleWindow ? kStaleThreshold : 0.0f;
    if (g_framesSinceReset < 1000) ++g_framesSinceReset;

    // Our textures match the game's render-target size; only the render rect is used.
    const resource_desc velocityDesc = dev->get_resource_desc(f.velocity);
    const format velocityFormat = VelocityViewFormat(velocityDesc.texture.format);
    const format depthFormat = DepthViewFormat(dev->get_resource_desc(f.depth).texture.format);
    {
        // The game's formats, logged once per change (they differ between games).
        static format logged[4] = {};
        const format now[4] = {dev->get_resource_desc(f.color).texture.format, velocityDesc.texture.format,
                               dev->get_resource_desc(f.depth).texture.format, dev->get_resource_desc(f.output).texture.format};
        if (memcmp(logged, now, sizeof(now)) != 0) {
            memcpy(logged, now, sizeof(now));
            LOG("dlss: input formats (DXGI): colour %u, velocity %u, depth %u, output %u", static_cast<unsigned>(now[0]),
                static_cast<unsigned>(now[1]), static_cast<unsigned>(now[2]), static_cast<unsigned>(now[3]));
        }
    }
    if (velocityFormat == format::unknown || depthFormat == format::unknown) {
        SetStatus("unsupported velocity or depth format (DXGI %u / %u)", static_cast<unsigned>(velocityDesc.texture.format),
                  static_cast<unsigned>(dev->get_resource_desc(f.depth).texture.format));
        return Result::skipped;
    }
    if (!EnsureTextures(dev, velocityDesc.texture.width, velocityDesc.texture.height)) return Result::skipped;
    const resource_view velocitySrv = GameSrv(dev, f.velocity, velocityFormat);
    const resource_view depthSrv = GameSrv(dev, f.depth, depthFormat);
    if (!velocitySrv.handle || !depthSrv.handle) {
        SetStatus("could not create views of the game's velocity / depth");
        return Result::skipped;
    }

    // From here on the game's command list is modified; the caller restores its state.
    // 1. Motion vectors and depth for DLSS.
    command_list* cmd = f.cmd;
    float constants[kConstants];
    memcpy(constants, f.clipToPrevClip, sizeof(float) * 16);
    constants[16] = float(f.renderW);
    constants[17] = float(f.renderH);
    constants[18] = 1.0f / float(f.renderW);
    constants[19] = 1.0f / float(f.renderH);
    constants[20] = cameraOnly ? 1.0f : 0.0f;
    constants[21] = staleThreshold;
    constants[22] = constants[23] = 0.0f;
    // Diagnostics slot for this frame: read what it held kStatsRing frames ago, clear it.
    const uint32_t slot = static_cast<uint32_t>(g_statsFrame++ % kStatsRing);
    CollectStats(dev, slot);
    cmd->barrier(g_stats[slot], resource_usage::unordered_access, resource_usage::copy_dest);
    cmd->copy_buffer_region(g_statsZero, 0, g_stats[slot], 0, kStatsWords * 4);
    cmd->barrier(g_stats[slot], resource_usage::copy_dest, resource_usage::unordered_access);

    cmd->bind_pipeline(pipeline_stage::compute_shader, g_pso);
    cmd->push_constants(shader_stage::compute, g_layout, 0, 0, kConstants, constants);
    const resource_view srvs[2] = {velocitySrv, depthSrv};
    const resource_view uavs[3] = {g_motionUav, g_depthUav, g_statsUav[slot]};
    cmd->push_descriptors(shader_stage::compute, g_layout, 1,
                          descriptor_table_update{{}, 0, 0, 2, descriptor_type::shader_resource_view, srvs});
    cmd->push_descriptors(shader_stage::compute, g_layout, 2,
                          descriptor_table_update{{}, 0, 0, 3, descriptor_type::unordered_access_view, uavs});
    cmd->dispatch((f.renderW + 7) / 8, (f.renderH + 7) / 8, 1);

    cmd->barrier(g_stats[slot], resource_usage::unordered_access, resource_usage::copy_source);
    cmd->copy_buffer_region(g_stats[slot], 0, g_statsReadback[slot], 0, kStatsWords * 4);
    cmd->barrier(g_stats[slot], resource_usage::copy_source, resource_usage::unordered_access);
    g_statsPending[slot] = true;
    g_statsAge[slot] = age;

    // Texture capture: the game's colour and velocity, and the motion / depth DLSS gets.
    if (g_captureArmed) {
        if (age == 0) g_captureSawReset = true;
        if (g_captureSawReset && age == kCaptureArmedAge) {
            g_captureArmed = g_captureSawReset = false;
            g_captureWanted = true;
        }
    }
    // Two consecutive frames: the second one's motion should map its colour onto the first's.
    const bool second = g_captureSecond;
    const char* captureOutput = nullptr;  // set when this frame is captured: DLSS output too
    if ((g_captureWanted && g_capture.empty()) || second) {
        captureOutput = second ? "output1" : "output0";
        g_captureWanted = false;
        g_captureSecond = !second;
        const char* sfx = second ? "1" : "0";
        CaptureTexture(cmd, f.color, resource_usage::shader_resource_non_pixel, (std::string("color") + sfx).c_str());
        CaptureTexture(cmd, f.velocity, resource_usage::shader_resource_non_pixel, (std::string("velocity") + sfx).c_str());
        CaptureTexture(cmd, g_motion, resource_usage::unordered_access, (std::string("motion") + sfx).c_str());
        CaptureTexture(cmd, g_depth, resource_usage::unordered_access, (std::string("depth") + sfx).c_str());
        char h[1024];
        int n = snprintf(h, sizeof(h),
                         "frame%s %llu age %u\nrender%s %ux%u output %u,%u %ux%u\njitter%s %.6f %.6f\npreexposure%s %.6g\n"
                         "camera_only %d stale_threshold %.3f ignore_velocity %d invert_motion %d\nclip_to_prev_clip%s",
                         sfx, static_cast<unsigned long long>(f.frame), age, sfx, f.renderW, f.renderH, f.outX, f.outY,
                         f.outW, f.outH, sfx, f.jitterX, f.jitterY, sfx, f.preExposure, cameraOnly ? 1 : 0,
                         staleThreshold, g_options.ignoreVelocity ? 1 : 0, g_options.invertMotion ? 1 : 0, sfx);
        for (int i = 0; i < 16 && n > 0 && n < int(sizeof(h)); ++i)
            n += snprintf(h + n, sizeof(h) - n, " %.9g", f.clipToPrevClip[i]);
        if (!second) {
            g_captureHeader.clear();
            g_captureFrame = f.frame;
        } else {
            g_captureWait = 6;
            g_captureStatus = "copied frames " + std::to_string(g_captureFrame) + "-" + std::to_string(f.frame) + ", writing...";
        }
        g_captureHeader += std::string(h) + "\n";
        LOG("capture: copied DLSS inputs of frame %llu (age %u)", static_cast<unsigned long long>(f.frame), age);
    }

    const resource ours[2] = {g_motion, g_depth};
    const resource_usage uav[2] = {resource_usage::unordered_access, resource_usage::unordered_access};
    const resource_usage srv[2] = {resource_usage::shader_resource, resource_usage::shader_resource};
    cmd->barrier(2, ours, uav, srv);

    // 2. DLSS into the game's TAAU output.
    auto list = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd->get_native());
    NVSDK_NGX_D3D12_DLSS_Eval_Params e = {};
    e.Feature.pInColor = reinterpret_cast<ID3D12Resource*>(f.color.handle);
    e.Feature.pInOutput = reinterpret_cast<ID3D12Resource*>(f.output.handle);
    e.pInDepth = reinterpret_cast<ID3D12Resource*>(g_depth.handle);
    e.pInMotionVectors = reinterpret_cast<ID3D12Resource*>(g_motion.handle);
    e.InJitterOffsetX = g_options.flipJitterX ? -f.jitterX : f.jitterX;
    e.InJitterOffsetY = g_options.flipJitterY ? -f.jitterY : f.jitterY;
    e.InRenderSubrectDimensions = {f.renderW, f.renderH};
    e.InReset = reset ? 1 : 0;
    e.InMVScaleX = g_options.invertMotion ? -1.0f : 1.0f;
    e.InMVScaleY = g_options.invertMotion ? -1.0f : 1.0f;
    e.InOutputSubrectBase = {f.outX, f.outY};
    // UE scales scene colour by View.PreExposure, which follows the eye adaptation and can
    // jump ~1000x at a cut. Without it DLSS blends history from another exposure scale.
    e.InPreExposure = g_options.passPreExposure ? f.preExposure : 1.0f;
    // UE's eye adaptation (1x1 RGBA32F, exposure in .r), as NVIDIA's UE plugin passes it
    // with auto exposure off.
    if (!key.autoExposure) e.pInExposureTexture = reinterpret_cast<ID3D12Resource*>(f.exposure.handle);
    const NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSS_EXT(list, feature->handle, g_params, &e);
    cmd->barrier(2, ours, srv, uav);
    if (NVSDK_NGX_FAILED(r)) {
        ++g_failures;
        SetStatus("DLSS evaluate failed (0x%08x)", static_cast<unsigned>(r));
        return Result::failed;
    }
    ++g_done;
    feature->lastUsed = g_done;
    g_lastEvalFrame = f.frame;
    g_lastFeature = feature;
    if (g_traceFrames) {
        --g_traceFrames;
        const auto d = [dev](resource r) { return dev->get_resource_desc(r).texture; };
        const auto c = d(f.color), z = d(f.depth), v = d(f.velocity), o = d(f.output);
        LOG("dlss: trace: frame %llu render %ux%u out %u,%u %ux%u%s%s | colour %llx %ux%u, depth %llx %ux%u, velocity "
            "%llx %ux%u, output %llx %ux%u | ours %ux%u",
            static_cast<unsigned long long>(f.frame), f.renderW, f.renderH, f.outX, f.outY, f.outW, f.outH,
            switched ? ", feature switched" : "", reset ? ", reset" : "", static_cast<unsigned long long>(f.color.handle),
            c.width, c.height, static_cast<unsigned long long>(f.depth.handle), z.width, z.height,
            static_cast<unsigned long long>(f.velocity.handle), v.width, v.height,
            static_cast<unsigned long long>(f.output.handle), o.width, o.height, g_texW, g_texH);
    }
    SetStatus("running: %ux%u -> %ux%u", f.renderW, f.renderH, f.outW, f.outH);

    // Frame generation experiment: the same motion and depth, tagged for DLSS-G (does
    // nothing unless Streamline is loaded). Like NGX it records natively; the caller
    // restores the game's state.
    if (fg::Loaded()) {
        fg::FrameInputs in;
        in.cmd = list;
        in.motion = reinterpret_cast<ID3D12Resource*>(g_motion.handle);
        in.depth = reinterpret_cast<ID3D12Resource*>(g_depth.handle);
        in.renderW = f.renderW;
        in.renderH = f.renderH;
        in.outW = f.outW;
        in.outH = f.outH;
        in.backBufferW = f.backBufferW;
        in.backBufferH = f.backBufferH;
        in.jitterX = e.InJitterOffsetX;
        in.jitterY = e.InJitterOffsetY;
        memcpy(in.viewToClip, f.viewToClip, sizeof(in.viewToClip));
        memcpy(in.clipToPrevClip, f.clipToPrevClip, sizeof(in.clipToPrevClip));
        memcpy(in.cameraPos, f.cameraPos, sizeof(in.cameraPos));
        memcpy(in.cameraFwd, f.cameraFwd, sizeof(in.cameraFwd));
        in.reset = reset;
        in.mvJittered = key.jittered;
        in.invertMotion = g_options.invertMotion;
        fg::TagFrame(in);
    }
    if (captureOutput) {
        cmd->barrier(f.output, resource_usage::unordered_access, resource_usage::unordered_access);  // DLSS done
        CaptureTexture(cmd, f.output, resource_usage::unordered_access, captureOutput);
    }

    // 3. Clamped copy of the DLSS output for next frame's SSGI/SSR (PatchLightingHistory),
    // and brightness counters. The pass runs for the counters even with the clamp off;
    // the copy is then just not handed to the lighting.
    g_histOutput = {};
    CollectLuma(dev, slot);
    if ((g_options.clampLightingHistory || g_options.logBrightness) && f.outX == 0 && f.outY == 0) {
        // Only an RGBA16F output (the clamped copy has to match it exactly), and a colour
        // format the clamp shader can read; check before creating any view.
        const resource_desc outDesc = dev->get_resource_desc(f.output);
        const format colorFormat = ColorViewFormat(dev->get_resource_desc(f.color).texture.format);
        const bool usable = outDesc.texture.format == format::r16g16b16a16_float && colorFormat != format::unknown;
        const resource_view colorSrv = usable ? GameSrv(dev, f.color, colorFormat) : resource_view{};
        const resource_view outUav = usable ? GameUav(dev, f.output, format::r16g16b16a16_float) : resource_view{};
        if (usable && colorSrv.handle && outUav.handle && EnsureHistory(dev, outDesc.texture.width, outDesc.texture.height)) {
            cmd->barrier(g_luma[slot], resource_usage::unordered_access, resource_usage::copy_dest);
            cmd->copy_buffer_region(g_statsZero, 0, g_luma[slot], 0, kStatsWords * 4);
            cmd->barrier(g_luma[slot], resource_usage::copy_dest, resource_usage::unordered_access);
            cmd->barrier(f.output, resource_usage::unordered_access, resource_usage::unordered_access);  // DLSS done
            const float clampConstants[4] = {float(f.outW), float(f.outH), float(f.renderW), float(f.renderH)};
            cmd->bind_pipeline(pipeline_stage::compute_shader, g_clampPso);
            cmd->push_constants(shader_stage::compute, g_clampLayout, 0, 0, 4, clampConstants);
            cmd->push_descriptors(shader_stage::compute, g_clampLayout, 1,
                                  descriptor_table_update{{}, 0, 0, 1, descriptor_type::shader_resource_view, &colorSrv});
            const resource_view clampUavs[3] = {outUav, g_histUav, g_lumaUav[slot]};
            cmd->push_descriptors(shader_stage::compute, g_clampLayout, 2,
                                  descriptor_table_update{{}, 0, 0, 3, descriptor_type::unordered_access_view, clampUavs});
            cmd->dispatch((f.outW + 7) / 8, (f.outH + 7) / 8, 1);
            cmd->barrier(g_luma[slot], resource_usage::unordered_access, resource_usage::copy_source);
            cmd->copy_buffer_region(g_luma[slot], 0, g_lumaReadback[slot], 0, kStatsWords * 4);
            cmd->barrier(g_luma[slot], resource_usage::copy_source, resource_usage::unordered_access);
            g_lumaPending[slot] = true;
            g_lumaFrame[slot] = f.frame;
            g_lumaAge[slot] = age;
            g_lumaClamp[slot] = g_options.clampLightingHistory;
            g_lumaPreExposure[slot] = f.preExposure;
            if (g_histHoldFrames) --g_histHoldFrames;
            else if (g_options.clampLightingHistory) {
                g_histOutput = f.output;
                g_histFrame = f.frame;
            }
        }
    }
    return Result::done;
}

void RequestCapture(bool afterNextCut) {
    if (!g_capture.empty()) return;
    if (afterNextCut) {
        g_captureArmed = true;
        // Pressed early in a shot (a short one like "Yah!"): take this shot.
        g_captureSawReset = g_framesSinceReset < kCaptureArmedAge;
        g_captureStatus = "armed: " + std::to_string(kCaptureArmedAge) + " frames into " +
                          (g_captureSawReset ? "this shot" : "the next shot");
        LOG("capture: armed for frame %u of %s", kCaptureArmedAge, g_captureSawReset ? "this shot" : "the next shot");
        return;
    }
    g_captureWanted = true;
    g_captureStatus = "waiting for the next DLSS frame";
}

std::string CaptureStatus() { return g_captureStatus; }

bool PatchLightingHistory(command_list* cmd, resource texture, bool compute, uint64_t frame) {
    if (!g_histOutput.handle || texture.handle != g_histOutput.handle || frame != g_histFrame + 1 ||
        g_histPatchedFrame == frame)
        return false;
    g_histPatchedFrame = frame;
    // copy_resource needs identical size and format; never copy over a texture that isn't
    // the one the clamped copy was made from.
    const resource_desc ours = g_device->get_resource_desc(g_hist), theirs = g_device->get_resource_desc(texture);
    if (ours.texture.width != theirs.texture.width || ours.texture.height != theirs.texture.height ||
        ours.texture.format != theirs.texture.format || ours.texture.depth_or_layers != theirs.texture.depth_or_layers ||
        ours.texture.levels != theirs.texture.levels) {
        LOG("dlss: lighting history copy skipped at frame %llu: ours %ux%u format %u, the game's %ux%u format %u",
            static_cast<unsigned long long>(frame), ours.texture.width, ours.texture.height,
            static_cast<unsigned>(ours.texture.format), theirs.texture.width, theirs.texture.height,
            static_cast<unsigned>(theirs.texture.format));
        g_histOutput = {};
        return false;
    }
    if (g_traceFrames)
        LOG("dlss: trace: lighting history copy at frame %llu into %llx (%ux%u)", static_cast<unsigned long long>(frame),
            static_cast<unsigned long long>(texture.handle), theirs.texture.width, theirs.texture.height);
    // The reader has the texture in SRV state for its stage; copy our clamped version over it.
    const resource_usage reading = compute ? resource_usage::shader_resource_non_pixel
                                           : resource_usage::shader_resource_pixel;
    const resource both[2] = {g_hist, texture};
    const resource_usage before[2] = {resource_usage::unordered_access, reading};
    const resource_usage copying[2] = {resource_usage::copy_source, resource_usage::copy_dest};
    cmd->barrier(2, both, before, copying);
    cmd->copy_resource(g_hist, texture);
    cmd->barrier(2, both, copying, before);
    g_patchedRing[frame % 8] = frame;
    if (++g_histPatches == 1) LOG("dlss: first clamped lighting history copy (frame %llu)", static_cast<unsigned long long>(frame));
    return true;
}

void OnSwapchainResize() {
    g_traceFrames = kTraceFrames;
    g_histHoldFrames = kHistHoldFrames;
    g_histOutput = {};
    LOG("dlss: swap chain resized: lighting history copy off for %u DLSS frames; tracing them", kHistHoldFrames);
}

void CancelLightingHistory() {
    if (!g_histOutput.handle) return;
    if (g_traceFrames)
        LOG("dlss: trace: lighting history copy cancelled (a TAAU dispatch fell back after DLSS)");
    g_histOutput = {};
}

void OnDestroyResource(resource r) {
    if (g_traceFrames && r.handle == g_histOutput.handle)
        LOG("dlss: trace: lighting history target %llx destroyed", static_cast<unsigned long long>(r.handle));
    if (r.handle == g_histOutput.handle) g_histOutput = {};
    if (auto it = g_uavs.find(r.handle); it != g_uavs.end()) {
        if (g_device) g_device->destroy_resource_view(it->second);
        g_uavs.erase(it);
    }
    auto it = g_srvs.find(r.handle);
    if (it == g_srvs.end()) return;
    if (g_device) g_device->destroy_resource_view(it->second);
    g_srvs.erase(it);
}

void Shutdown(device* dev) {
    if (dev != g_device || !g_device) return;
    for (auto& feat : g_features) NVSDK_NGX_D3D12_ReleaseFeature(feat.handle);
    g_features.clear();
    g_lastFeature = nullptr;
    g_wanted = g_failedKey = {};
    if (g_params) NVSDK_NGX_D3D12_DestroyParameters(g_params);
    g_params = nullptr;
    for (auto& [res, view] : g_srvs) dev->destroy_resource_view(view);
    g_srvs.clear();
    for (auto& [res, view] : g_uavs) dev->destroy_resource_view(view);
    g_uavs.clear();
    DestroyHistory(dev);
    if (g_clampPso.handle) dev->destroy_pipeline(g_clampPso);
    if (g_clampLayout.handle) dev->destroy_pipeline_layout(g_clampLayout);
    g_clampPso = {};
    g_clampLayout = {};
    DestroyTextures(dev);
    DestroyStats(dev);
    DestroyLuma(dev);
    ReleaseCapture();
    if (g_pso.handle) dev->destroy_pipeline(g_pso);
    if (g_layout.handle) dev->destroy_pipeline_layout(g_layout);
    g_pso = {};
    g_layout = {};
    if (g_init == Init::ok) NVSDK_NGX_D3D12_Shutdown1(g_d3d);
    g_init = Init::untried;
    g_device = nullptr;
    g_d3d = nullptr;
    LOG("dlss: shut down");
}

std::string Status() {
    char buf[320];
    snprintf(buf, sizeof(buf), "%s (%llu frames, %llu failures)", g_status.c_str(),
             static_cast<unsigned long long>(g_done), static_cast<unsigned long long>(g_failures));
    return buf;
}

uint64_t FramesDone() { return g_done; }
uint64_t LightingHistoryPatches() { return g_histPatches; }

}  // namespace dlss
