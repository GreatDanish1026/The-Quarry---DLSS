// DLSS Super Resolution for The Quarry on Direct3D 12, recorded into the game's own
// command list in place of its TAAU dispatch. See dlss.cpp.
#pragma once

#include <reshade.hpp>

#include <cstdint>
#include <string>

namespace dlss {

using namespace reshade::api;

// Everything DLSS needs from one TAAU dispatch.
struct Frame {
    command_list* cmd = nullptr;
    resource color, depth, velocity, output;  // game textures, already in SRV / UAV state
    resource exposure;                         // TAAU t0: UE's 1x1 eye adaptation (exposure in .r), may be 0
    uint32_t renderW = 0, renderH = 0;         // render view rect (top-left 0,0)
    uint32_t outX = 0, outY = 0, outW = 0, outH = 0;  // output view rect
    float jitterX = 0, jitterY = 0;            // render pixels, from the TAA constants
    float clipToPrevClip[16] = {};             // View.ClipToPrevClip, row-major
    float preExposure = 1.0f;                  // View.PreExposure: the colour input is scaled by it
    // For frame generation (fg.cpp) only:
    float viewToClip[16] = {};                 // View.ViewToClip (rows 28-31), row-major, jittered
    float cameraPos[3] = {};                   // View.WorldCameraOrigin (row 67)
    float cameraFwd[3] = {};                   // view forward (row 18)
    uint32_t backBufferW = 0, backBufferH = 0;
    bool reset = false;                        // camera cut (sticky across fallback frames)
    uint64_t frame = 0;                        // probe frame number, to spot frames DLSS missed
};

enum class Result {
    skipped,  // nothing recorded: let the game's TAA run
    failed,   // commands were recorded but DLSS failed: restore state, let the game's TAA run
    done,     // DLSS wrote the output: restore state, skip the game's TAA
};

struct Options {
    bool enabled = true;
    bool flipJitterX = false;
    bool flipJitterY = false;
    bool mvJittered = false;  // DLSS feature flag; changing it recreates the feature
    bool invertMotion = false;  // DLSS MV scale -1: tests the motion vector sign
    bool ignoreVelocity = false;  // camera reprojection only: tests the game velocity decode
    // Give SSGI/SSR a clamped copy of the DLSS output. Off by default since 1.1.1: it only
    // matters with screen-space lighting mods, and its extra views and copies are the riskiest
    // part of the add-on (both device-removal bugs so far were in it).
    bool clampLightingHistory = false;
    bool passPreExposure = true;  // tell DLSS the game's per-frame pre-exposure
    bool gameExposure = true;  // give DLSS the game's eye adaptation instead of its auto exposure (recreates)
    // DLSS model preset hint (0 default, 5 E, 10 J, 11 K, 12 L, 13 M); recreates the feature.
    // J by default: K (the default for Quality) ignores the current frame for ~70 frames in the
    // "Yah!" shot (FINDINGS.md). An NVIDIA App / driver override takes precedence.
    uint32_t preset = 10;
    bool logBrightness = false;  // one log line per DLSS frame: input / DLSS / clamped luma
    bool verboseLog = false;     // motion statistics after resets and every 60 frames
};
extern Options g_options;

// Every present: NGX init and DLSS feature (re)creation happen here, on ReShade's
// immediate command list, never inside the game's command lists.
void Tick(effect_runtime* runtime);
// At a TAAU dispatch: records DLSS only if a feature for exactly these sizes exists;
// otherwise asks Tick for one and returns skipped without touching the command list.
Result Evaluate(const Frame& f);
// Before a pass that reads <texture>: if it's last frame's DLSS output (read back by the
// game's SSGI/SSR), copy the clamped version over it first. Once per frame.
bool PatchLightingHistory(command_list* cmd, resource texture, bool compute, uint64_t frame);
uint64_t LightingHistoryPatches();
// Copies the next DLSS frame's colour, velocity, motion and depth to files next to the exe.
void RequestCapture(bool afterNextCut = false);  // or 45 frames after the next camera cut
std::string CaptureStatus();
void OnDestroyResource(resource r);  // drops cached views of game textures
void OnSwapchainResize();            // holds off the lighting copy, logs the next DLSS frames (F11 hang)
// A TAAU dispatch fell back to the game's TAA: next frame's lighting copy target can't be
// trusted any more (the game may reuse that texture), so it's dropped.
void CancelLightingHistory();
void Shutdown(device* dev);

std::string Status();  // one line for the overlay and logs
uint64_t FramesDone();

// Set by the probe: writes a line to QuarryDLSS.log.
extern void (*g_log)(const char* fmt, ...);

}  // namespace dlss
