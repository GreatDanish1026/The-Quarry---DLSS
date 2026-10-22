# Renderer findings

Steam build 11105655, exe PE timestamp `0x64493c74`, UE 4.26.2. Probe 0.1.0, run of
2026-10-15 (`QuarryDLSS_frame_2751.txt`, gameplay at 3840x2160).

## Device and output

- **Direct3D 11**, feature level 11_0. Without `-dx12` the game doesn't use D3D12.
- Swap chain: 3840x2160 `R10G10B10A2_UNORM`, flip-discard (`SwapEffect 4`), tearing
  allowed (`Flags 0x802`), windowed.
- During startup the game creates throwaway devices on the RTX 5080, the AMD iGPU and
  WARP before it creates the real device on the RTX 5080.
- Gameplay renders **letterboxed**: the view rect is 3840x1600 inside 3840x2160 render
  targets, and the final pass writes it at y offset 280. The screen percentage is 100%
  (`sg.ResolutionQuality=100`), so TAA runs without upsampling.

## Direct3D 12 (`-dx12`)

Probe 0.2 run on 2026-10-15 with `-dx12` in the launch options:

- **The D3D12 RHI works.** The game ran for about 2.5 minutes (8,700 frames) and didn't
  crash. Swap chain: 3840x2160 `R10G10B10A2_UNORM`, 3 buffers. It creates throwaway
  D3D12 devices at startup, the same way it does on D3D11.
- **Same shaders on both APIs.** The D3D12 shader keys match the D3D11 ones (for example
  `89b573cf3e394331`, `f0db50aa69e703c1`), so UE 4.26 ships SM5 DXBC for D3D12 too, not
  DXIL. The DXBC declaration parser works on both.
- Probe 0.2 found no temporal passes and no constants on D3D12, because it didn't decode
  descriptor tables. Probe 0.3 does (below).
- This matters for frame generation: with a working D3D12 path, Streamline can run
  natively. We wouldn't need the D3D11/D3D12 shared-resource bridge that Days Gone uses.

### Probe 0.3 on D3D12: bindings and jitter

Run of 2026-10-15 with `-dx12` (`QuarryDLSS_cbuffers_9300.txt`, gameplay, camera moving):

- The descriptor-heap mirroring works. The probe finds the same temporal passes with the
  same shader keys as on D3D11, including the main TAA `2f606fcaa114ea67` with the same
  slots: t0 exposure, t1 colour, t2 depth, t4 stencil, t5 history, u0 output. Its
  first sighting (frame 67, loading) had a 1x1 dummy in t3 (velocity), so velocity is
  only bound once the game writes it. The D3D11 gameplay frame had the real
  RGBA16_UNORM velocity there.
- The D3D12 adapter is identified: RTX 5080. As on D3D11, the game also creates
  throwaway devices on the AMD iGPU and WARP.

**Main TAA constants** (`2f606fcaa114ea67`):

- `cb0` (29 float4) holds the TAA pass parameters:
  - row 3 = InputSceneColorSize (3840, 2160, 1/3840, 1/2160);
  - row 4 = (half texel, max viewport UV): 0.7405 = 1599.5/2160, the letterboxed view;
  - row 0.y = 0.04, the current-frame weight;
  - **row 28.xy = jitter in pixels** (`TemporalJitterPixels`), with y pointing down the
    screen.
- `cb1` (145 float4 declared) is UE's View uniform buffer:
  - **row 126 = `View.TemporalAAJitter`**: .xy is this frame's jitter and .zw last
    frame's, in clip units. Clip = (px × 2/3840, −py × 2/1600), divided by the 3840x1600
    view rect, not the 3840x2160 texture.
  - rows 28-31 = ViewToClip (the projection). It's reversed-Z with an infinite far plane,
    the near plane at 10 cm (row 31.z), focal length 1.6667 (about 62° horizontal FOV)
    and aspect 3840/1600. Jitter is folded into row 30.xy.
  - rows 0-3 = TranslatedWorldToClip, rows 4-7 = WorldToClip. Rows 73, 85 and 90
    carry the previous frame's jitter, so they're the previous-frame matrices.

**Jitter sequence:** Halton(2,3) minus 0.5, **8 phases**, indices 1-8 then repeating:
(0, −0.167), (−0.25, 0.167), (0.25, −0.389), (−0.375, −0.056), (0.125, 0.278),
(−0.125, −0.278), (0.375, 0.056), (−0.4375, 0.389). That's UE's default
`r.TemporalAASamples=8`. DLSS wants more phases at lower render scales, so the mod has
to raise this, as Days Gone does.

**Feeding DLSS:** jitter offset = `cb0` row 28.xy (pixels). Check the sign once in-game
with the DLSS jitter debug overlay. Depth is reversed-Z (`DepthInverted`), with near =
10 and an infinite far plane. Motion vectors are UE's velocity buffer, which only covers
moving objects, plus camera motion reconstructed from depth and the previous-frame
matrices in `cb1`.

### Render scale can't be set from Engine.ini [SystemSettings]

Run of 2026-10-15 (`-dx12`) with `r.ScreenPercentage=67` and `r.TemporalAA.Upsampling=1`
under `[SystemSettings]` in the user `Engine.ini`. **No effect:** the view rect stayed at
3840x1600 (main TAA still dispatched 480x200 groups, and `cb0` still had a 3840x2160
input with 0.7405 max V). The game sets the screen percentage itself. Its own settings
(`[/Script/SMGRuntime.GamePCSettings]` in GameUserSettings.ini) include
`bUseDynamicResolution`, `VRSQuality` and `RTReflectionsQuality` / `bRTAmbientOcclusionEnabled`,
so the D3D12 build also has ray-traced options.

A second run with the same cvars also under `[ConsoleVariables]` (higher priority)
**also had no effect**. At 4K the main TAA still dispatched 480x200 for a 3840x1600 view
rect, and at the 1280x720 window size 160x67 for 1280x536. The ini approach is dead and
the test Engine.ini was removed. **The mod has to set the screen percentage in-engine**,
as Days Gone writes the cvar data directly each frame, or by hooking whatever the game
uses to set it.

### Console variables in memory

From the exe's own code (cvar registration and lookup sites next to
`L"r.ScreenPercentage"` / `L"r.TemporalAA.Upsampling"`):

- `IConsoleManager::Singleton` is at **exe+0x709acd0**. Every lookup loads it with
  `mov rcx, [rip+x]`, and probe 0.4 finds it from that pattern in 16 ms.
- IConsoleManager vtable: +0x10 `RegisterConsoleVariable(float)`, +0x18
  `RegisterConsoleVariable(int)`, **+0xb0 `FindConsoleVariable(name, bTrackFrequentCalls)`**.
- IConsoleVariable vtable: **+0x58 `AsVariableInt`**, **+0x60 `AsVariableFloat`**, each
  returning `TConsoleVariableData<T>*` (game-thread value at [0], render-thread at [1]).
- The `TAutoConsoleVariable` for r.ScreenPercentage is at exe+0x70eeff8 (vptr, Target
  +8, Ref +0x10), and r.TemporalAA.Upsampling at exe+0x71f06c0.

Probe 0.4 writes r.ScreenPercentage (default 67) and r.TemporalAA.Upsampling = 1 into both
copies every frame, logs any time the game changes them back, and has an overlay checkbox
and slider.

### TAAU path at 67% (probe 0.4, `-dx12`, `QuarryDLSS_frame_1354.txt`)

**Forcing the cvar data works.** The game's own values were r.ScreenPercentage 100 and
r.TemporalAA.Upsampling **1** (TAAU was already on, so it just runs at 100%). The game
writes 100 back on swap-chain resizes and loading. The probe re-forces it the next frame,
so the mod has to keep doing that too.

| Pass | Runs at |
|---|---|
| Base pass, lighting, `cd75f114186bff1a`, `b569efd647217872` (322x134 groups), translucency composite `04346c4e90a9704d` | **2576x1072** render rect in 2576x1448 textures |
| **Main TAA, now the upscaler: `6435b4eb8190e22b`** (480x200 groups) | 2576x1072 in -> **3840x1600** out |
| Downsample `ba3e1c97e7c22acb`, histogram, eye adaptation, bloom, tonemapper `4b7e40416e1f163b`, final passes | 3840x1600 |

The 67% render rect is rounded to 2576x1448 textures. Render targets are 8-aligned, so
the pass still sees 2576x1072 valid pixels.

**DLSS replaces exactly `6435b4eb8190e22b`** (and the other TAAU permutations at other
sizes):

| Slot | Texture | DLSS input |
|---|---|---|
| t0 | 1x1 RGBA32F | exposure (`pInExposureTexture`, or use auto-exposure) |
| t1 | 2576x1448 RGBA16F, output of `04346c4e90a9704d` | **colour** (render res, subrect 2576x1072) |
| t2 | 2576x1448 R32F view of R32G8X24 | **depth** (reversed-Z) |
| t3 | 2576x1448 RGBA16_UNORM, written by geometry | **velocity**: real in gameplay, encoded UE4 velocity (needs decoding plus camera motion for static pixels) |
| t4 | 2576x1448 stencil view | (responsive AA, not needed by DLSS) |
| t5 | 3840x1600 RGBA16F | game history (DLSS keeps its own) |
| u0 | **3840x1600 RGBA16F** | **DLSS output** |

**TAAU constants** (`QuarryDLSS_cbuffers_20783.txt` and `_21082.txt`, 67%, camera moving),
`6435b4eb8190e22b` `cb0` (29 float4):

| Row | Contents |
|---|---|
| 2 | ints (2576, 1448, 1, 0): input texture size |
| 3, 5, 19 | (2576, 1448, 1/2576, 1/1448): input texture size |
| 4 | (half texel, max UV): 0.73999 = 1071.5/1448 |
| 20 | ints (0, 0, 2575, 1071): input min/max pixel coord |
| 21, 23 | (3840, 1600, 1/3840, 1/1600): output viewport size |
| 24 | (0, 0, 3840, 1600): output viewport rect |
| **27** | **(2576, 1072, 1/2576, 1/1072): render view rect size** |
| **28** | **.xy = jitter in render pixels**, .zw = (0.6708, 1.4907): resolution fraction 2576/3840 and its inverse |

- `cb1` row 126 (`View.TemporalAAJitter`) now scales by 2/2576 and −2/1072. That's the
  **render** view rect.
- **More jitter phases with TAAU:** the two 8-frame dumps continue the Halton(2,3)
  sequence past index 8 (indices 2-16 seen, consecutive). This matches UE 4.26 scaling
  r.TemporalAASamples by 1/fraction² when upsampling (8/0.67² → about 18). The DLSS
  mod can use the engine's own sequence as is, read from `cb0` row 28 each frame.
- Not seen yet: the TAAU permutation at other output sizes (the 1280x720 window).

### First DLSS runs (0.5.0 / 0.5.1, `-dx12`)

- **0.5.0 crashed at launch.** DLSS feature creation failed mid-frame on the game's
  command list (`0xbad00002`, platform error) while the TAAU inputs were still 1x1
  loading placeholders. The game then crashed in its D3D12 RHI on the same thread
  (exe+0x297c976, reading offset 0x18 of a null compute shader pointer).
- **0.5.1 runs DLSS.** NGX loads the driver's `_nvngx.dll` from the DriverStore, and the
  app's `nvngx_dlss.dll` 310.9.1 is overridden by the **NVIDIA App's DLSS override**
  (snippet 310.9.16, model preset 11). Features were created for 2576x1072 -> 3840x1600
  (letterboxed gameplay) and 2576x1448 -> 3840x2160 (full-frame scenes). Evaluates ran
  with no failures and no crash. Loading frames use the game's TAAU, because velocity
  is a 1x1 placeholder.
- **Feature thrash:** switching between letterboxed and full-frame shots recreates the
  feature (wait_idle plus creation, about 30-100 ms). Fix: create one feature for the
  full 3840x2160 output with output subrects enabled, and pass the letterbox as the
  output subrect.

### DLSS smearing: motion vectors check out, camera cuts suspected (0.5.3)

- 0.5.1 showed heavy smearing, stair-stepped echoes and rectangular blocks of stale
  image during a cutscene (ReShade screenshot 15:40:58, DLSS in steady state).
- 0.5.2 removed the device at launch. The stats buffer's typed UAV was given a byte
  size, but **ReShade passes buffer view offset/size straight through as D3D12
  FirstElement/NumElements** (elements). Fixed in 0.5.3.
- 0.5.3 motion stats (render pixels, every 4th pixel): during pans the mean is 8-39 px
  and the max 22-100 px with 100% of pixels moving; still camera about 0.01 px; depth
  0.003-0.17; 14-49% of pixels carry object velocity. The magnitudes are sane. The sign
  matches UE's FSR2 integration (MV = (-0.5 W Vx, +0.5 H Vy), current -> previous).
- View UB rows used for cut detection: **row 18 = view forward** (= row 60), **row 67 =
  WorldCameraOrigin** (cm). 0.5.4 resets DLSS on the TAA's bCameraCut **or** on a camera
  jump over 2 m or a turn over 15 degrees in one frame, and logs each reset with its
  reason and every overlay option change.

### 0.5.4 run: DLSS looks right (2026-10-15 17:20-17:22)

- User report: "it appears to be working" through a cutscene and free movement, default
  options (no overlay changes logged), no DLSS failures, no crash.
- **The game does raise bCameraCut on its cuts:** 30 of 31 resets were the game's flag
  (with matching camera jumps of 34 cm to 376 m or turns up to 170 degrees). The jump
  detector added only one reset (frame 1338, 24 m, no flag). So the missing-cut theory
  doesn't explain the earlier 0.5.1 smearing screenshot. Its cause is unconfirmed; watch
  for it in the same cutscene.
- Feature thrash is still there: letterboxed <-> full-frame changes recreate the
  feature 6 times in this run.
- **0.5.5 caches features per size** (up to 4, LRU) instead of recreating them. One
  feature can't cover both shapes: DLSS output size is fixed at creation, output subrects
  only offset it, and the letterboxed TAAU output texture is itself 3840x1600. Switching
  to a feature that wasn't used last frame sets InReset.

### "And scene." corruption is DLSS-specific (0.5.5, 18:36-18:41)

- The prologue shot "And scene." (doorway in the middle, dark walls either side)
  corrupts reproducibly with DLSS on (screenshots 15:40:58 and 18:36:14). The walls fill
  with bright, stair-stepped copies of the doorway.
- A/B at the same shot: **DLSS off + 67% forced: clean** (18:40:22, 18:40:25). **Both
  off (100%): clean** (18:41:15). So it's not the forced render scale.
- Stats for the shot: mean motion about 1 px, but max 25-64 px (the echo step size), and
  45% of pixels carry game velocity, far more than the two characters.
- Hypothesis: the game's velocity isn't encoded as I decode it (e.g. UE's optional
  sqrt "gamma" encoding), inflating tiny motions. 0.5.6 logs decoded vs camera-only
  motion for object-velocity pixels and adds "Ignore game velocity, camera motion only
  (test)".

### Velocity decode is correct; stale velocity right after cuts (0.5.6, 18:43-18:45)

- Decoded game velocity matches camera-only reprojection on object-velocity pixels
  (0.37 vs 0.38 px, 0.36 vs 0.39, 0% inflated). The linear UE 4.26 decode is right.
- "And scene." was **clean with DLSS on** this time, both with game velocity (18:44:27)
  and camera-only (18:45:01). The corruption is intermittent.
- In every run, the frames right after the cut into this shot have **stale game
  velocity**: decoded 34-43 px vs camera 3 px with 20-40% inflated (0.5.6), and max 298 px
  right after the reset in the corrupted 0.5.5 run. Probable cause of the intermittent
  corruption: DLSS is reset only on the cut frame, then fed previous-shot velocities.
  Proposed guard: camera-only motion for a few frames after a reset, plus a reset on the
  frame after the cut.
- **0.5.7 adds the guard:** DLSS InReset on the cut frame and the next one, and
  camera-only motion for 4 frames after any reset (game cut flag, camera jump or feature
  switch).
- **0.5.7 didn't fix it:** corrupted again at "Yah!" (18:49:16, 0.1 s after the cut at
  15.908). In the ~60 frames after that cut, object pixels averaged **47 px decoded vs
  4.6 px camera** (37% inflated, max 309 px), back to 4.88 vs 4.91 in the next window. So
  the bad velocity lasts many frames, not 1-2.
- **0.5.8:** double reset and 2 camera-only frames, then for 90 frames per pixel: game
  velocity more than 24 px away from camera motion is replaced by it. Logs each of the 40
  frames after a reset (decoded vs camera, % inflated, % replaced, max motion).
- **Root cause found in the 0.5.8 run (18:52):** corrupted again at the "Yah!" kick
  close-up (18:52:33.69, about 80 frames after the cut at 32.923) with small motion (mean
  2.2 px, max 12 px). The per-frame lines after the cuts at frames 1582 and 1679 are
  **missing**. Those cut frames never reached DLSS: they fell back to the game's TAAU
  (fallback logging was capped at 10 per session, used up during loading), and since
  bCameraCut is only set on the cut frame, **the reset was lost**. DLSS then carried the
  previous shot's history into the new one. This also explains why the earlier
  "fixes" seemed partly effective: it depended on whether the cut frame passed
  validation.
- **0.5.9:** cuts stay pending until DLSS actually evaluates; any frame DLSS missed
  (fallback, unknown permutation, feature not ready) resets it on the next evaluate
  (frame-gap check); fallback reasons are logged on every change of reason.
- **0.5.9 run (18:55):** resets now reach DLSS. Every cut frame falls back (an input
  smaller than the render rect, most likely UE's 1x1 velocity placeholder on cut frames)
  and the next frame evaluates with reset+0. **But the kick close-up still corrupts**
  (18:55:42.745, 0.83 s after a reset that reached DLSS, motion about 0.5 px, max 1.2 px).
  So lost resets were a real bug but not the whole cause. Something produces echoes
  within a shot with almost no motion. Next: a frame dump during that shot, to look for
  passes that read back the TAAU output (it doubles as the game's TAA history).
- 0.5.10: fallback reasons are specific (which input is too small, etc.) and no longer
  include per-frame values, so they're logged once per change.
- Cut frames confirmed: UE binds a **1x1 velocity** placeholder on every camera-cut frame.

### Feedback loop: the TAAU output is read back into the next frame's lighting

Frame dump 1705 (0.5.10, kick close-up, 85 frames after a reset that reached DLSS):

- The TAAU history `t5` (`...565a0`) is the previous frame's TAAU output (DLSS output when
  DLSS runs; the output ping-pongs between two 3840x1600 RGBA16F textures). Besides the
  TAAU it is read early in the next frame by **#82 CS `67dca9330bbd3383`** and the lighting
  composite **#112 PS `cd75f114186bff1a`** (as t6). That output becomes the scene colour
  DLSS upscales.
- These are UE 4.26 screen-space effects lighting the scene from the previous frame's
  final colour, enabled by the pak mods: **`NativeSSGI_P.pak`** (`r.SSGI.Enable=1`,
  `Quality=3`, `HalfRes=0`; it also sets `r.setres=1280x720`, which explains the 1280x720
  windows) and **`BetterReflections_P.pak`** (`r.SSR.Quality=4`,
  `r.SSR.HalfResSceneColor=0`, SSGI quality per scalability level).
- With UE's TAA the loop is bounded by neighbourhood clamping. DLSS has no such clamp,
  so a slightly misplaced or brighter copy re-enters every frame. That fits the
  staircases with almost no motion and the blown-out brightness in every corrupted
  screenshot.
- 0.5.11 adds runtime test switches through the cvar access: "Disable SSGI"
  (`r.SSGI.Enable 0`) and "Disable SSR" (`r.SSR.Quality 0`).
- **0.5.11 run (19:00-19:02):** game values were r.SSGI.Enable 1, r.SSR.Quality 4. With
  both forced to 0 before the cutscene and DLSS on, the "And scene." shot was **clean,
  with normal brightness** (19:01:46). All corrupted shots had been blown out. It's one
  run of an intermittent problem, so it's strong evidence for the feedback-loop cause
  but not yet proof. Options: turn SSGI/SSR off under DLSS, or break the loop by giving
  the lighting a clamped copy of the DLSS output.
- **0.5.12 keeps SSGI/SSR and breaks the loop:** right after DLSS, `dlss_clamp.hlsl`
  writes a copy of the DLSS output clamped to the 3x3 min/max of the current input
  colour (render pixels), the same idea as UE's TAA history clamp. In the next frame,
  just before the first pass that reads last frame's DLSS output (detected generically
  from the mirrored bindings, normally #82), that copy is copied over the texture, with
  SRV -> copy_dest -> SRV barriers for the reader's stage. The displayed image stays
  unclamped. Overlay: "Clamp lighting history" (on by default) with a copy counter.
- **0.5.12 run (2026-10-16, 08:00-08:02):** SSGI/SSR left on (no override or option
  changes logged), clamp on. The user replayed the "And scene." kick shot 5 times and
  the corruption did not appear. The 08:02:26 screenshot is clean with normal
  brightness. The first clamped copy happened at frame 317, and the history texture
  followed the 3840x1600 and 3840x2160 output sizes. There was no device removal, crash
  or barrier error. The only fallbacks were the known loading and cut frames (1x1
  velocity) and one-frame "not ready" on new sizes. Conclusion: the SSGI/SSR feedback
  loop was the remaining cause of the smearing, and the clamped history fixes it while
  keeping both effects on.
- **0.5.12 second run (09:51-09:52): corruption came back** with SSGI/SSR on and the
  clamp on. The first "And scene." of the session was blown out (09:51:57), including the
  dark side walls, with horizontal staircases in the lower left. The clamp was copying
  (first copy at frame 277), and motion was sane: the shot started at the frame 2247 cut,
  with only 2-6% inflated velocity after it, not the stale-velocity pattern. So the
  conclusion above was premature: 6 clean runs out of 7 isn't proof for an intermittent
  bug. The clamp also can't bound a loop that runs through the input colour, since SSGI
  adds last frame's light to the very input it clamps against. It only removes DLSS's
  own accumulation from the loop. Open question: does the blow-up live in DLSS's
  internal history or in the game's buffers (SSGI/SSR denoiser histories)?
- **0.5.13 brightness log:** the clamp pass also measures luma on every 4th output pixel
  in each direction. It logs one line per DLSS frame:
  `luma f<frame> age <frames since reset>: in <mean> max | dlss <mean> max | clamped
  <mean> max | cut <% of samples the clamp lowered by >20%> hot <% luma > 16, dlss/in>
  nonfinite <dlss/in> lit <yes|no|off>`. Means clip each sample at 64. "lit" says whether
  the lighting read the clamped copy in the next frame. Reading it: if `dlss` climbs
  first while `in` stays flat, DLSS's own history is blowing up. If `in` climbs first,
  the energy comes through the game's lighting. Non-finite counts flag NaN/Inf.
  Overlay: "Log brightness every frame" (on by default).
- **0.5.13 run (10:03-10:04):** the brightness log points to pre-exposure, not the
  lighting loop:
  - **Kick shot (cut at frame 2072):** the game's input colour jumped from mean luma
    0.017 to 31.4 (max ~61000, near the fp16 limit) in one frame, at age 3 (frame 2076).
    It then decayed over ~30 frames to ~0.5 and settled near 0.12. DLSS tracked the input
    exactly (the clamp cut ~0%, no NaN/Inf). A one-frame jump of ~1800x can't be SSGI
    feedback. It's the input's scale changing.
  - **"Yah!" shot (frames 1981-2071, the 10:04:08 screenshot):** stretched staircase
    smears. The input mean wandered 0.094 -> 0.058 -> 0.177 -> 0.069, while DLSS's mean
    stayed flat at ~0.087. The clamp cut rose from 0% to 30% of samples. DLSS was holding
    history from another brightness scale. Motion vectors were sane (decoded ~4 px vs
    camera ~2.7 px).
  - **Cause:** View row 135 is (NumSceneColorMSAASamples, **PreExposure**,
    OneOverPreExposure). It was 11.42 / 0.0876 in the 15 Oct TAAU dump. UE 4.26 stores
    scene colour multiplied by PreExposure, which follows the eye adaptation. UE's TAA
    rescales its history when it changes; DLSS can only do that if it's told through
    `InPreExposure`, which we left at 1. This explains the brightness that persists in
    static areas. It also explains why runs vary, since it depends on the adaptation at
    the cut, and why SSGI/SSR (which change the scene's exposure) made it likelier.
- **0.5.14:** passes View.PreExposure (cb1 row 135.y) as `InPreExposure` every frame,
  with an overlay toggle "Pass the game's pre-exposure to DLSS" (on). The luma log lines
  now show `pre <value>`.
- **0.5.14 run (10:08):** the log confirms the kick-shot jump is the game's pre-exposure.
  `pre` went 0.428 -> 2458 at age 3 after the frame 1674 cut, the same frame the input
  mean went 0.017 -> 31. It then decayed with the eye adaptation (1016 at age 15, 17 at
  31, ~8 at 64). DLSS followed the input throughout (clamp cut 0.0%). There was no
  screenshot of the kick shot this run.
- **The "Yah!" smear is a separate bug (10:08:38 screenshot, same as 10:04:08):**
  pre-exposure stayed constant at ~0.427 through the whole shot (frames 1584-1673), so
  it isn't exposure. As in 0.5.13, the DLSS mean stays flat (~0.09) while the input moves
  0.094 -> 0.059 -> 0.176 -> 0.068. The clamp cut climbs steadily from 0% at the reset to
  ~30% by age 55, like a smear front advancing each frame. The streaks are on the left
  near wall and the floor behind it, while the face is clean. Velocity stats look sane
  (76% object-velocity pixels, decoded ~1.4x camera motion, nothing over the stale
  threshold). Suspect a region whose motion we get wrong: for example, scenery that
  moves with the camera and gets camera motion, or vice versa. Next test: the same shot
  with "Ignore game velocity" and with "Invert motion vectors".
- **0.5.14 A/B run (10:11-10:12):**
  - **"Yah!" shot:** both "Ignore game velocity" (frames 6047-6138) and "Invert motion
    vectors" (frames 11820-11912) kept the clamp cut at **0.0%** for the whole shot. DLSS
    tracked the input (0.06 -> 0.16 -> 0.07), where the default run reached 30% with a
    flat DLSS mean. So the smear comes from the game's velocity on part of that shot.
    Camera-only motion fixes it. Inverting probably works only because DLSS then rejects
    that history, so it's not a fix.
  - **Kick shot:** the 10:12:38 screenshot is fully white. It was taken at age ~3 of the
    frame 11913 cut, exactly when `pre` jumps 0.428 -> 2458 and the input mean
    0.017 -> 31. DLSS matches the input, so this white flash is in the game's colour,
    not added by DLSS. The 10:11:55 screenshot (ignore velocity, same shot ~0.4 s later)
    is clean and normally exposed. Still open: does the game flash like this with DLSS
    off?
- **0.5.15:** logs `pre-exposure <value> at frame N (DLSS on|off)` whenever the game's
  PreExposure changes by more than 25%, with DLSS on or off. A DLSS-off run of the kick
  shot answers whether the jump to ~2458 is the game's own.
- **0.5.15 run (10:15), DLSS off from frame 3097: the white flash is the game's own.**
  The user saw it flash white with the game's TAAU. The log shows the same sequence as
  under DLSS: a cut at frame 4772, then `pre-exposure 2458` at 4776 (DLSS off), decaying
  1857, 1348, 995 ... 12.6 at 4819, then ~2.6 by 5062. The values at cuts are identical
  in every run (2458, 1.2, 0.3446), which looks like authored or clamped cutscene
  exposure, not something we cause. **The kick-shot blow-out is closed as far as DLSS is
  concerned.** DLSS now reproduces the game's flash (with `InPreExposure`) instead of
  holding the bright history for seconds.
- **Remaining DLSS bug: the "Yah!" smear**, from the game's velocity on part of that
  shot (camera-only motion fixes it).
- The 0.5.15 frame dump mid-"Yah!" (frame 2293, age 84) shows only that the velocity
  texture (RGBA16_UNORM) comes from scene-geometry draws. Pass lists can't show velocity
  values. So 0.5.16 adds **Ctrl+F8 "Capture DLSS input textures"**: at the next DLSS
  frame it copies the game's colour and velocity and our motion and depth to readback
  buffers. A few presents later it writes `QuarryDLSS_capture_<frame>_<name>.bin`
  (rows tightly packed) plus a `.txt` header next to the exe. The header has the
  sizes, DXGI formats, render/output rects, jitter, pre-exposure, guard state and
  ClipToPrevClip, enough to recompute camera motion per pixel offline.
- 0.5.16's first captures (frames 1934 and 2253) wrote headers but no textures. Map
  was given a read range of rows x RowPitch, but the last row isn't padded, so the
  range overran the buffer. 0.5.17 maps the whole buffer and logs any failure. Both
  captures also landed in the kick shot: "Yah!" lasts only ~90 frames. 0.5.17 adds
  **Ctrl+Shift+F8**, which arms a capture 45 frames after the next camera cut.
- `tools/analyze_capture.py` recomputes the decoded game velocity and the camera
  reprojection per pixel and writes PNGs. **Kick shot (frame 1926):** static walls with
  game velocity match camera motion to within 0.02 px, so the velocity decode and the
  ClipToPrevClip reprojection are right. Only the moving characters differ.
  **"Yah!" (frame 1635, age 45):** 71% of pixels carry game velocity. The wall again
  matches the camera (0.02 px). The arm and shoulder report 2-6 px (+x, i.e. "was further
  right") against 0.8 px of camera motion. That's plausible for a moving arm, so one
  frame can't say whether it's wrong.
- **0.5.18:** a capture now takes two consecutive DLSS frames (suffixes 0 and 1). The
  analyser checks each candidate motion field (game, camera-only, inverted game, zero,
  what DLSS got) by reprojecting frame 1 into frame 0 and measuring the colour error.
  The best match is the motion that's actually right.
- **Method check (kick shot, frames 6531-6532):** on the 14.7% of pixels where the game's
  velocity disagrees with camera motion, the reprojection error was 0.238 for game
  velocity, 0.397 camera-only, 0.387 zero and 0.447 inverted. So the game's velocity is
  right for the moving characters there, and the test separates right from wrong
  motion. The armed capture missed "Yah!" again, because it was pressed just after that
  cut. 0.5.19 arms for frame 45 of the current shot if it's younger than that.
- **"Yah!" pair (frames 1613-1614, age 45-46): the game's velocity is right here too.**
  On the 21.8% of pixels where it disagrees with camera motion (the arm and shoulder,
  up to 8.8 px/frame), the reprojection error is 0.144 for game velocity, 0.301 for
  camera-only, 0.327 for zero and 0.367 inverted. The wall matches the camera. So the
  motion vectors DLSS gets are correct. "Ignore game velocity" and "Invert" only looked
  clean because wrong motion makes DLSS throw its history away. They aren't fixes.
- The error maps for every candidate are bright on the face and shoulder. The lighting
  itself changes frame to frame: the shadow sweeps across the face, and the shot's
  input mean swings 0.06 -> 0.18. Where the luma log shows DLSS's mean staying flat
  (~0.09) through those swings, DLSS is keeping history through a lighting change.
- **0.5.20** also captures the DLSS output (`output0/1`). The analyser downsamples it and
  compares it with the input per region (`_outdiff.png`), to show where and how the
  output departs from the frame it was made from.
- **"Yah!" pair with output (frames 1680-1681):** the DLSS output departs from its
  input in a few clear ways:
  - The input has a hard shadow across the lower face, but the output shows the face
    fully lit (region mean 0.027 in vs 0.127 out).
  - The arm sits in an older position, with wall texture streaked through the dark
    forest behind it.
  - The bright wall and the lit areas match (|d| ~0.08).
  
  The motion vectors are verified correct (0.117 reprojection error vs 0.30 camera-only),
  and 0.5.3 confirmed their sign in fast pans. So DLSS is over-trusting history where the
  image is dark. That's typical of a wrong exposure for HDR input: dark differences get
  compressed below DLSS's rejection threshold.
- **0.5.21:** "Use the game's exposure, not DLSS auto exposure" (on by default). DLSS
  gets the TAAU's t0, UE's 1x1 RGBA32F eye adaptation (exposure in .r), as
  `pInExposureTexture`, and the feature is created without `AutoExposure`. This is what
  NVIDIA's UE plugin does with auto exposure off. It's only used when t0 is 1x1
  RGBA32F. The feature key includes the exposure mode, so toggling recreates the
  feature. Test: in the luma log for "Yah!", the clamp cut should stay near 0% instead
  of climbing to ~30%.
- **0.5.21 run (10:36): no change.** The feature was created with game exposure, and the
  "Yah!" smear looked identical (10:36:23 screenshot). In the luma log, the DLSS mean
  sits at 0.094-0.097 from age 5 to 72 while the input moves 0.059-0.176, and the clamp
  cut reaches 31%. **At age 73 (frame 1829) DLSS snaps back to the input in one frame**
  (cut 24.6% -> 0.1%) with no reset from us. So DLSS is effectively ignoring the current
  frame for ~70 frames, then recovering by itself.
- The captured inputs (frames 1680/1681) are clean: motion has no NaN/Inf and a max of
  14 px, nothing is written outside the render rect, there are no saturated velocity
  codes, and depth is 0..0.26. Camera-only or inverted motion avoids the freeze for the
  whole frame, including the face, whose motion barely differs. So the large
  near-camera arm motion seems to make the model reject the current frame globally.
- **The model is Preset K** (transformer), set by a driver/NVIDIA App (DRS) override:
  NGX logs "Using DRS Overridden Preset K" for Quality (M for Perf, L for Ultra Perf).
  **0.5.22** adds a model-preset selector (Default, E (CNN), J, K, L, M). The hint is
  applied to every quality mode at feature creation, and changing it recreates the
  feature. The DRS override wins, so it only takes effect with the NVIDIA App's DLSS
  model override for The Quarry turned off.
- **0.5.22 run (10:41-10:43), NVIDIA App override off: the "Yah!" smear is Preset K's.**
  NGX logged "Using App hint Preset E", then "Preset J". The user saw no smear over
  several attempts with each. In the luma log the clamp cut stayed at 0% (E, frame 2395)
  and at most 0.1% (J, frames 6711 and 12547) for the whole shot, against ~31% under K.
  Our inputs are the same in every case. Preset K over-trusts history when a large
  near-camera object moves fast against a dark background. **0.5.23 requests Preset J
  by default.** The overlay can still pick E, K, L or M. An NVIDIA App model override
  for the game replaces our hint, so it should be off.
- **0.5.23 playtest (2026-10-16): working well.** The user reports DLSS looks good across
  different scenes and cutscenes on Preset J, with the defaults: pre-exposure and game
  exposure passed, the clamped lighting history on. Still open: whether the lighting
  clamp is needed now that the real causes are fixed, and turning the probe into a
  release build (diagnostics off by default, smaller log).

### 0.6.0: release build

DLSS behaves as in 0.5.23. User-facing changes:

- **Overlay:** "QuarryDLSS" (formerly "QuarryDLSS probe"). The top level shows only:
  - DLSS on/off;
  - a plain status line ("Active: 2576x1072 upscaled to 3840x1600.", "Starting: load
    into the game.", "Not available: DLSS needs DirectX 12...");
  - a quality mode: Quality 66.7%, Balanced 58%, Performance 50%, Ultra Performance
    33.3%, or Custom.

  **Advanced** has the DLSS model (J recommended, then K, L, M, E, driver default), the
  custom render scale and "Stabilise screen-space lighting" (the clamped history).
  Everything else moved to a collapsed **Diagnostics** section.
- **Settings are saved** in ReShade.ini, `[QuarryDLSS]`: Enabled, QualityMode,
  CustomScale, Preset, StabiliseLighting.
- **DLSS off now restores the game's own render scale** (native TAA). The old behaviour,
  67% with the game's TAAU, is the Diagnostics option "Keep the render scale with DLSS
  off".
- **Quieter log:** the per-frame brightness log, the reset+N and 60-frame motion
  statistics and the pre-exposure lines are off by default (Diagnostics: "Verbose log",
  "Log brightness every frame"). Resets, fallbacks and feature creation are still
  logged.
- **Debug hotkeys** (Ctrl+F8/F9/F10) are off by default (a Diagnostics checkbox). The
  buttons are there too.
- The "game changed a forced value" check now compares against the value last
  written, so changing the quality mode isn't reported as the game's doing.
- **0.6.1:** adds **Ultra Quality (77%)**, created as NGX `UltraQuality` (render/output
  fraction above 0.72). It never actually ran DLSS: 77% uses another TAA permutation
  (fixed in 1.0.0-fg.4), and NGX rejects the `UltraQuality` mode at creation (fixed in
  1.0.0-fg.5). See "Frame Generation spike", runs 3 and 4. The quality mode is now saved by name (`Quality=UltraQuality`,
  `Quality`, `Balanced`, ...). A 0.6.0 `QualityMode` index is migrated, since
  inserting a mode at the top shifted the indices.

### Crash report from a probe 0.3.1 session

A `-dx12` session at 14:57-14:58 (probe 0.3.1) crashed about 9 s after a frame dump:
`EXCEPTION_ACCESS_VIOLATION writing 0x5cc3` at **exe+0x4055e62**, with only game frames
on the stack. The add-on was loaded but not on the stack. The install has about 20 crash
reports from before this project too, so it's probably the game or the pak mods, but
this needs watching.

### Ctrl+F11 toggled the game to windowed mode

UE toggles fullscreen on F11, so every Ctrl+F11 frame dump also switched the game to its
1280x720 window size (`ResolutionSizeX/Y` in GameUserSettings.ini) and back. That's why
dumps coincide with swap chain resizes, some dumps are at 1280x720, and some contain two
frames' worth of passes. Probe 0.3.1 dumps on **Ctrl+F9**.

## Frame Generation spike (branch `fg-spike`, 1.0.0-fg.1)

The question: can NVIDIA DLSS Frame Generation (DLSS-G, through the Streamline SDK) run
in The Quarry from our ReShade add-on on D3D12? This build adds an opt-in, off-by-default
experiment in `src/fg.cpp`. **Nothing has been run in game yet**, so the in-game answers
below are still open. What's established so far comes from the Streamline 2.14.1 SDK
(source, headers and docs, downloaded 2026-10-16 from NVIDIA-RTX/Streamline) and from the
build.

### Why it's hard: who owns the swap chain

- The game already presents through ReShade's swap chain proxy (ReShade is `dxgi.dll` in
  the game folder). DLSS-G needs **Streamline's** proxy swap chain in the present path. It
  intercepts Present, presents the generated frames from its own queue and thread, and
  makes the game render into off-screen buffers ("the host has no direct access to swap
  chain buffers", DLSS-FG guide §18).
- The add-on can't link `sl.interposer.lib`. It exports `CreateDXGIFactory*` /
  `D3D12CreateDevice` and would clash with `dxgi.lib`, and the add-on must load without
  Streamline. So Streamline runs in **manual hooking** mode: `sl.interposer.dll` is
  loaded at run time with `LoadLibrary` (only after `verifyEmbeddedSignature` passes),
  and every entry point comes from `GetProcAddress` / `slGetFeatureFunction`.
  `dumpbin /dependents` confirms the add-on still imports only dxgi, advapi32, user32
  and kernel32.
- From the SL source (`sl.api/sl.cpp`): `slUpgradeInterface` accepts an `ID3D12Device`,
  an `IDXGIFactory` and also an **already-created `IDXGISwapChain`** (it wraps it in a new
  proxy). Calling it is the easy part. The hard part is getting the game's calls into the
  proxy, because the game holds ReShade's proxy, which holds the native swap chain.
- Streamline's plugins (`sl.common` hooks `slHookCreateSwapChain*`, and DLSS-G's binary
  plugin) expect to see the swap chain **being created**. The DLSS-G plugin isn't open
  source, so whether it can attach to a swap chain that already exists can't be read
  from the code; the test answers it.

### Two strategies, picked in ReShade.ini (`[QuarryDLSS] FrameGeneration=0/1/2`, restart)

| | 1: factory hook | 2: swap chain upgrade |
|---|---|---|
| How | At `init_device` the add-on patches the shared `IDXGIFactory` vtable (system32 dxgi.dll) slots 10 `CreateSwapChain` and 15 `CreateSwapChainForHwnd`. When the **game exe** creates its swap chain, the call goes through an SL factory proxy (`slUpgradeInterface` on the factory). Streamline's call back down passes through (thread-local guard), so ReShade still wraps the native swap chain. | On the first ReShade present after `init_swapchain`: `slUpgradeInterface` on the native swap chain, then the native swap chain vtable is patched (Present, GetBuffer, SetFullscreenState, ResizeBuffers, Present1, GetCurrentBackBufferIndex, ResizeBuffers1). Calls coming **from ReShade's module** on that swap chain go to the SL proxy; Streamline's own calls go to DXGI. |
| Order | game -> ReShade -> Streamline -> DXGI, because ReShade wraps the factory and calls the real one (run 1). It would be game -> Streamline -> ReShade if the game called the real factory directly | game -> ReShade -> Streamline -> DXGI |
| DLSS-G sees creation | yes | no |
| Back buffers | Streamline's from the start | The game and ReShade already hold the real ones. Toggle F11 twice (a resize) so they fetch them again through Streamline |
| ReShade sees | real frames only. In the game-first order it would see generated frames too; `fg::IsRealPresent` then counts one probe frame per game Present, so the DLSS SR frame-gap reset logic still works | real frames only |
| ReShade overlay | drawn before Streamline, so it gets interpolated | drawn before Streamline, so it gets interpolated |

Both strategies restore every patched vtable entry when the add-on unloads. That matters
because ReShade unloads and reloads the add-on for the throwaway devices at startup.
Streamline is only initialised once a swap chain is being created (factory) or exists
(upgrade), so the throwaway devices never load it.

### What DLSS-G gets (TagFrame, called right after the DLSS SR evaluate)

- **Tags** (`slSetTagForFrame`, frame-based tagging): our depth `g_depth` (R32F,
  reversed-Z) and motion vectors `g_motion` (RG16F, render pixels, current -> previous,
  camera motion everywhere), both with the render rect as extent and in UAV state. The
  lifecycle is `eOnlyValidNow` by default: Streamline copies them on the game's command
  list, because we overwrite both every frame. The overlay can switch to
  `eValidUntilPresent`. The **back buffer subrect** is tagged with the view centred in
  the back buffer (3840x1600 at y 280 in letterboxed shots). Streamline then copies the
  bars outside it unchanged instead of interpolating them.
- **Constants** (`slSetConstants`):
  - `cameraViewToClip` = View rows 28-31 with row 30.xy (the TAA jitter) zeroed, and
    `clipToCameraView` = its inverse;
  - `clipToPrevClip` = View rows 122-125, and `prevClipToClip` = its inverse. UE builds
    ClipToPrevClip from the NoAA projections, so it has no jitter;
  - `jitterOffset` = the same pixels given to DLSS SR (TAAU cb0 row 28.xy);
  - `mvecScale` = (1/renderW, 1/renderH), negated with "Invert motion vectors";
  - `cameraPos` = row 67, `cameraFwd` = row 18, and right/up derived from it in UE's
    Z-up frame (roll ignored);
  - near = row 31.z (10 cm), far = 1e7 (infinite), FOV and aspect from rows 28/29;
  - `depthInverted`, `cameraMotionIncluded`;
  - `reset` from our cut detection (the same value DLSS SR gets).
- **Reflex/PCL**: `slReflexSetOptions(eLowLatency)` and `slDLSSGSetOptions` are applied on
  the present thread, as Streamline asks. Around the game's Present: `eRenderSubmitEnd`
  and `ePresentStart`, the present, then `ePresentEnd`. Then a new frame token,
  `slReflexSleep` and `eSimulationStart/End` + `eRenderSubmitStart` for the next frame.
  There's no hook into UE's game thread, so the simulation markers are placeholders and
  the sleep stalls the RHI thread, not the simulation.
- HUD-less colour and UI alpha are **not** provided, so subtitles and QTE prompts get
  interpolated with the scene.

### Log lines to read after a test (all prefixed `fg:` or `sl`)

- `fg: settings: ...`, then `patched IDXGIFactory2::CreateSwapChainForHwnd (was <module>+...)`
  (factory) or `swap chain upgraded to an SL proxy` + the seven patches (upgrade).
- `CreateSwapChain from <caller>`: the caller is "ReShade" (as in run 1) or "the game
  exe", and either now engages the hook. It should be followed by `CreateSwapChain
  through Streamline: 0x00000000`.
- `DLSS-G: loaded 1, supported: eOk, SL 2.14.1, NGX ...`, the requirements line (HW
  scheduling, VSync, driver), and the `sl error/warn` lines from Streamline itself.
- `DLSS-G status 0x0 ok`, or flags such as `reflex-not-detected` or `constants-invalid`.
- Every ~2 s: `real frames <fps, mean, sd, p99, max, hitches> | below SL <same for every
  frame Streamline presents> | SL presented N (x per real frame)`. With 2x working,
  "below SL" should be about twice "real", with a similar spread. These are CPU timestamps
  of the submits; FrameView's `MsBetweenDisplayChange` is the reference for display
  pacing.

### Setup for a test run

1. Hardware-accelerated GPU scheduling on (Windows Settings > Display > Graphics). DLSS-G
   requires it.
2. `.\build.ps1 -Install -InstallStreamline`. This copies sl.interposer, sl.common,
   sl.dlss_g, sl.reflex, sl.pcl and nvngx_dlssg from `third_party\streamline\bin\x64`
   (production, signed DLLs from the SDK's `bin\x64`, gitignored like nvngx_dlss.dll).
3. ReShade overlay > QuarryDLSS > "Frame generation (experiment)": choose the factory hook,
   then restart the game. DLSS SR must be on, since the tags are taken at the DLSS SR
   dispatch.
4. Load into gameplay and a cutscene. Note whether generated frames appear (NVIDIA overlay
   or FrameView FPS roughly doubling), any pink tint (DLSS-G's invalid-state overlay),
   subtitle and QTE prompt artifacts, and crashes. Then try 3 and 4 generated frames on the
   RTX 5080.
5. If the factory hook doesn't engage or crashes: switch to "Swap chain upgrade", restart,
   load in, press F11 twice and check the same things.

### Run 1 (1.0.0-fg.1, factory hook, 2026-10-16 13:45): Streamline never loaded

- User report: no generated frames; lower performance and the GPU not fully used.
- The log shows why. The factory vtable was patched (the saved originals are
  `dxgi.dll+0x2c4d0` / `+0x2d8f0`; a standalone test program confirms these are the
  system DXGI functions, so ReShade doesn't patch that vtable). But the game's one
  `CreateSwapChain` (legacy, not `ForHwnd`: 3840x2160 R10G10B10A2, 3 buffers, flip-discard,
  flags 0x802) reached our hook **from ReShade** (ReShade.log "Redirecting
  IDXGIFactory::CreateSwapChain" at 03.178, ours at 03.226). ReShade wraps the game's
  factory and then calls the real one. The hook only engaged for calls from the game exe,
  so it passed the call through: no `slInit`, no tags, no Reflex.
- So nothing from the experiment ran per frame in this session. The performance drop
  can't come from Streamline or DLSS-G; that still needs an A/B with the hook off.
- **1.0.0-fg.2:** the factory hook also engages when ReShade is the caller. That gives
  game -> ReShade -> Streamline -> DXGI, the best order: Streamline sees the creation and
  hands out its buffers, ReShade sees only real frames, and ReShade's overlay gets
  interpolated. Streamline's call back down still passes through (thread-local guard) to
  the real DXGI function. Log callers now read "ReShade" or "the game exe", since both
  modules are named dxgi.dll.

### Run 2 (1.0.0-fg.2, factory hook, 2026-10-16 13:50-13:51): DLSS-G works

- **User report: frame generation works, with no visible artifacts.**
- The hook engaged as intended: `CreateSwapChain from ReShade`, then `CreateSwapChain
  through Streamline: 0x00000000`, so the order is game -> ReShade -> Streamline -> DXGI.
  Streamline coexists with ReShade this way.
- `slInit`, then DLSS-G, Reflex and PCL all report loaded and supported (driver 617.42, HW
  scheduling required and on). Status `0x0 ok` from the first present. Max generated frames
  is **5** on the RTX 5080 (up to 6x).
- **Generated frames and pacing** (the 2-second log lines in gameplay):

  | Mode | Real frames | Presented by SL | Real frame time sd / max |
  |---|---|---|---|
  | 3x (2 generated) | 52.0 fps | 156 fps (3.00 per real frame) | 0.2-1.4 ms / 20-29 ms |
  | 2x (1 generated, switched live) | 78.0 fps | 156 fps (2.00 per real frame) | 0.2-0.8 ms / 13-16 ms |

  The output stays at ~156 fps in both modes, so it looks capped (probably Reflex/DLSS-G
  pacing to the display refresh). Switching 3x -> 2x applied on the next present with no
  hitch. "Below SL" pacing isn't measured in this order: ReShade sits above Streamline,
  so nothing of ours sees the generated presents. FrameView or the NVIDIA overlay would
  have to cover display pacing.
- **Cut frames:** each camera cut logs `Failed to find global tag 'kBufferTypeDepth' /
  'kBufferTypeMotionVectors'` (and once "Unable to find 'common' constants"). These are
  the frames where DLSS SR falls back to the game's TAAU (1x1 velocity on cuts), so
  nothing is tagged and DLSS-G skips generation for that frame. That's harmless and
  arguably right at a cut. Loading screens log "missing common constants" every frame
  for the same reason (280 lines in this run). It would be quieter to switch DLSS-G off
  while DLSS SR isn't running.
- **NVIDIA App overrides apply:** with the App's DLSS override (DRS `SL_DLSS_OVERRIDE`)
  on, Streamline loaded its OTA plugins (sl.common/dlss_g/reflex/pcl **2.14.16** from
  `C:\ProgramData\NVIDIA\NGX\models`) instead of our 2.14.1 copies, and NGX logged
  "Feature dlssg override enabled". It used the App's frame generation snippet
  (`dlssg\versions\20318480`), even though `eAllowOTA` isn't set.
- **Frame generation presets:** there are none to expose. Streamline 2.14.1's
  `DLSSGOptions` has mode, frame count, flags and dynamic-MFG fields but no preset or
  model hint. The SDK's lettered presets (A-M) are only for DLSS Super Resolution
  (`DLSSPreset`) and Ray Reconstruction (`DLSSDPreset`), and the NGX headers have no
  frame generation preset parameter either. The frame generation model comes from the
  driver, or from the NVIDIA App's override as above.

- **1.0.0-fg.3:** the overlay checkbox and frame-count slider become one "Frame
  generation" choice: Off / 2x / 3x / 4x / Dynamic (`DLSSGMode::eDynamic`, where
  Streamline picks the multiplier to reach the refresh rate; it falls back to 2x when
  `bIsDynamicMFGSupported` is false). It's saved as `FrameGenerationMode=<name>`, and
  fg.1/fg.2's `FrameGenerationOn` / `FrameGenerationFrames` are migrated. Off now sets
  `eRetainResourcesWhenOff`, so switching back on doesn't stutter.

### Run 3 (1.0.0-fg.3, 2026-10-16 14:10): 4x and Dynamic work; Ultra Quality had no DLSS

- **User report: 4x and Dynamic work well; subtitles and QTE prompts show no visual
  issues.**
- **4x:** 39 real fps -> 156 presented (4.00 per real frame, real frame sd 0.3 ms).
  **Dynamic** settled on 2x here: 78 -> 156. Output is held at ~156 fps in every mode
  (display refresh pacing), so above the needed multiplier a fixed 3x/4x only lowers the
  real frame rate (52, then 39 fps), and with it responsiveness. Dynamic picks the
  smallest multiplier that reaches the refresh rate, which makes it the sensible default.
- **Bug: Ultra Quality turned frame generation off.** In fact it turned DLSS Super
  Resolution off, and has done since Ultra Quality was added in 0.6.1. At 77% UE 4.26
  uses another TemporalAA permutation (the screen percentage band 71-100%):
  **`2f606fcaa114ea67`**, 2960x1664 inputs, 3840x1664 output texture, 3840x1600 view.
  The add-on only knew `6435b4eb8190e22b` (the 50-71% band: Quality 67%, Balanced 58% and
  Performance 50% all use it). So at Ultra Quality the game's own TAAU ran: no DLSS
  frames, nothing tagged ("tagged 0"), and DLSS-G skipped every frame. The NGX line
  "(UltraQuality) Using App hint Preset J" in earlier logs isn't evidence: NGX logs the
  hint for every quality mode whenever a feature is created. Ultra Performance (33%)
  has never been seen in a log and is probably a third permutation.
- **1.0.0-fg.4:** `2f606fcaa114ea67` is a known TAAU permutation. Any other permutation is
  tried when it's the probe's current main TAA guess and that guess upscales by texture
  size. That covers Ultra Performance without knowing its key, and the log names the key
  the first time. `BuildDlssFrame` now refuses any pass whose render rect isn't smaller
  than its output ("not upscaling"). So the 100% TAA (`2f606` again) and the SSGI denoiser
  `b569efd647217872`, which has the same shape and is the main guess at 100%, are never
  replaced.

### Run 4 (1.0.0-fg.4, 2026-10-16 14:16): the second Ultra Quality bug

- User report: still no frame generation at Ultra Quality.
- fg.4 now treats `2f606fcaa114ea67` as the TAAU, but DLSS feature creation for
  2960x1232 -> 3840x1600 failed with **`0xbad00010` (`NVSDK_NGX_Result_FAIL_UnsupportedParameter`)**.
  A failed size isn't retried, so the game's TAAU ran from then on ("DLSS not ready for
  this size", about 600 fallbacks per 4 s). The cause: `QualityFor` sent any fraction
  above 0.72 to `NVSDK_NGX_PerfQuality_Value_UltraQuality`, and the NGX runtime (310.9.x,
  through the NVIDIA App override) doesn't accept that mode. So 0.6.1's "created as NGX
  `UltraQuality`" never worked either; it failed at creation.
- **1.0.0-fg.5:** fractions above 0.62 (up to DLAA at 0.9) are created as `MaxQuality`
  (Quality). DLSS takes any render size; the mode only selects its tuning. This also
  covers custom render scales above 72%. Ultra Quality stays in the menu (the user asked
  to keep it now that it can work). Not yet confirmed in game.

- **Run 5 (1.0.0-fg.5, 2026-10-16):** Ultra Quality works, with DLSS SR and frame generation
  on (user report). Earlier the same day 4x and Dynamic were reported working well, with no
  issues on subtitles or QTE prompts.

### 1.0.0-fg.6: release clean-up (for 1.1)

- **Factory hook only.** The swap chain upgrade path and the hook choice are gone. Frame
  generation is one setting, `FrameGenerationMode` = Off / 2x / 3x / 4x / Dynamic, Off by
  default. The factory hook (and so Streamline) is only installed when it's on at launch
  and all six Streamline files are next to the exe. Turning it on from Off asks for a
  restart; turning it off takes effect live (`eOff` with `eRetainResourcesWhenOff`). The
  fg.1/fg.2 keys still migrate. Reflex low latency is on whenever frame generation is (no
  separate switch, since DLSS-G requires it).
- **Paused without DLSS SR.** If the frame being presented wasn't tagged (DLSS off, loading
  screens, camera-cut frames where DLSS falls back), DLSS-G is set to Off for that
  present and back on with the next tagged one. That ends the per-frame "missing common
  constants" / "Failed to find global tag" errors. Pauses of 30+ frames are logged and shown
  ("Paused: needs DLSS Super Resolution running"); single cut frames aren't.
- **Resize and fullscreen safety.** The SL swap chain's ResizeBuffers, ResizeBuffers1 and
  SetFullscreenState are wrapped: DLSS-G is switched off before the call (from that
  thread) and stays off for 3 presents, as NVIDIA's guide asks, to avoid a Present
  deadlock. Not yet exercised in game: F11 or alt-tab with frame generation on.
- **Quieter log.** Streamline's info lines and the 2-second pacing lines only appear with
  Diagnostics > Verbose log. A repeating Streamline warning or error is logged 3 times,
  then every 1000th time with a count.
- **Overlay.** The main panel has "Frame generation" right under the DLSS quality mode,
  with a tooltip and one status line ("Active: 2x, 78 -> 156 fps.", "Paused: ...",
  "Restart the game to turn on frame generation.", "Not installed: ..."). It's greyed out
  while DLSS SR is off or the Streamline files are missing. The technical status, the
  pacing line and the tag-copy switch moved to Diagnostics.

### Run 6 (1.0.0-fg.6, 2026-10-16 14:32): F11 twice hung the GPU

- User report: F11 twice with frame generation on; going back to fullscreen, the game
  died with UE's "Map failed ... DXGI_ERROR_DEVICE_REMOVED, reason DXGI_ERROR_DEVICE_HUNG".
- Log: the first F11 (ResizeBuffers to 1280x720 at 56.87) went through. fg.6 switched
  DLSS-G off and back on 3 presents later, and DLSS-G set itself up for 1280x720 at 57.53
  ("Override Reported ... FrameGeneration"). The second F11 (ResizeBuffers back to
  3840x2160 at 57.63) was followed by only 11 frames in 6 s, then the device was removed
  at 14:33:03.7. ReShade logged the removal too.
- Likely causes, both in fg.6's resize handling:
  - DLSS-G was switched Off with `eRetainResourcesWhenOff` and back on 3 presents after
    the 4K resize, still holding the resources it made for 1280x720. NVIDIA's guide asks
    for `slFreeResources` after anything but a brief pause.
  - The back buffer subrect in the tags came from the probe, which only refreshes at the
    next ReShade present, so the first frames after the resize were tagged with the old
    1280x720.
- **1.0.0-fg.7:**
  - After a resize or fullscreen change, DLSS-G goes Off. Its resources are freed
    (`slFreeResources(kFeatureDLSS_G)`) on the present thread once a present has gone
    through with it Off, and it stays Off for 10 presents and until that free has
    happened. It then reallocates for the new size.
  - The back buffer size for the subrect tag now comes from the SL swap chain itself
    (`GetDesc`), read at creation and right after each resize.
  - Brief pauses (cut frames, DLSS SR fallbacks) still keep the resources, since the size
    hasn't changed.
  - Not yet confirmed in game.

### Run 7 (1.0.0-fg.7, 2026-10-16 14:36): the F11 hang isn't DLSS-G generating frames

- User report: same device-hung crash on F11 twice.
- What the log shows:
  - During loading (DLSS SR idle, nothing tagged), a full F11 cycle (SetFullscreenState,
    resizes to 1920x1080 and back to 3840x2160) went through.
  - In gameplay (DLSS SR running, Ultra Quality, frames tagged), F11 resized to 1280x720
    at 39.27. DLSS-G was switched off and its resources freed at 39.46. It **never came
    back on**, since the 10-present wait hadn't run out.
  - F11 back to 3840x2160 at 40.37. 11 frames were recorded in 1.2 s, including one at
    100% while the game reset the scale. **No present completed after that resize**: the
    first one returned the device-removed error at 45.69.
- So the GPU hangs on the first 4K frames after a resize back up, with DLSS-G off. Run 6
  had the same shape: shrinking survives, growing back while DLSS SR is running doesn't.
  Two suspects:
  - DLSS SR's own resize path, which has never been tested with F11 since DLSS went in.
    It recreates the motion, depth and lighting-history textures on a size change.
  - Streamline's per-frame copy of the volatile tags (`eOnlyValidNow`), recorded into the
    game's command list even while DLSS-G is paused. If its clone buffers stay at the
    988x556 size, copying 2960x1664 into them overruns, which fits "shrink fine, grow
    hangs". The earlier "unsupported alignment ... overridden to 0" warning is from that
    clone path.
- Next tests: (a) F11 twice with frame generation Off from launch (no Streamline), DLSS
  on; (b) frame generation on with Diagnostics > "Streamline copies motion vectors and
  depth when tagged" unchecked (`eValidUntilPresent`, so no clone copies).

### Run 8 (1.0.0-fg.7, 2026-10-16 14:39-14:42): the F11 hang is DLSS Super Resolution's

- 14:39: F11 cycles with DLSS off were fine. With DLSS on, and frame generation set to
  Off live (Streamline still loaded), F11 hung as before.
- 14:42, frame generation **Off from launch** (no Streamline loaded), DLSS on at Ultra
  Quality: F11 to 1280x720 at 05.08 (DLSS created 988x412 -> 1280x533 and ran), F11 back to
  3840x2160 at 06.29 (DLSS reused the cached 2960x1232 -> 3840x1600 feature, recreated the
  motion/depth and lighting-history textures and ran). 11 frames later the game wrote back
  100% for a frame ("not upscaling" at 07.61), then the GPU hung. **Alt-tab doesn't hang**:
  it doesn't resize the swap chain.
- So the hang is in the DLSS SR path, on the first frames after the swap chain grows. It
  isn't frame generation, and it's probably in 1.0.0 too: F11 with DLSS on was never
  tested before. Same shape in runs 6 and 7: device lost about 4 s after the
  "not upscaling" frame.
- Prime suspect: the stabilised-lighting copy (`PatchLightingHistory`). It copies the
  clamped DLSS output over the game texture that last frame's DLSS output went to, before
  the game's SSGI/SSR reads it. `copy_resource` needs identical sizes and formats, and
  barriers assume an SRV state, across a resize that reallocates every game texture.
- **1.0.0-fg.8** (diagnostic):
  - the copy checks size, format, depth and mips against the target and skips (logged)
    on any mismatch;
  - for 30 DLSS frames after each resize, every evaluate is logged (rects, the colour,
    depth, velocity and output textures with sizes, feature switch, reset), along with
    every lighting copy and the destruction of its target.
  - Test 1: F11 twice with the defaults. Test 2, if it still hangs: the same with Advanced
    > "Stabilise screen-space lighting" off, which turns off the clamp pass and the copy.

### Run 9 (1.0.0-fg.8, 2026-10-16 14:45): stabilised lighting is the F11 trigger

- With Advanced > "Stabilise screen-space lighting" **off**, F11 twice in gameplay with DLSS
  on **didn't hang**. With it on, all four earlier F11 tests hung.
- The trace shows what's unusual after the resize back up: in probe frame 1208 the game
  dispatched its TAAU **twice before one present**. First at 77% (DLSS replaced it: output
  `...60ed0` 3840x1664), then at 100% ("not upscaling", the game's own TAA ran), the frame
  where the game writes r.ScreenPercentage back to 100 before we force 77 again. Runs 6-8
  hung ~4 s after that same 100% frame.
- The lighting copy assumes one DLSS frame per present. At the next frame's first lighting
  pass it copies the clamped image over the texture DLSS wrote last frame, with barriers
  that assume that texture is in its SRV state. After the extra 100% rendering, UE's
  render-target pool can have handed that texture out again. The copy then lands on a
  texture in another state or in use, which fits the hang. (fg.8's size check alone
  wouldn't catch it: the size and format can still match.)
- **1.0.0-fg.9:**
  - any TAAU dispatch that falls back to the game's TAA cancels the pending lighting copy
    (`CancelLightingHistory`, called from every fallback);
  - a swap chain resize cancels it and holds it off for 30 DLSS frames.
  - The clamp pass and brightness counters still run; only the copy over the game's
    texture is skipped.
  - Not yet confirmed: F11 twice with stabilise on.

### Run 10 (1.0.0-fg.9, 2026-10-16 14:51): F11 fixed

- **User report: F11 works with DLSS SR and frame generation on and "Stabilise
  screen-space lighting" checked.**
- Log: F11 cycles at 14:51:10-17 (loading) and 24.8 / 25.5 (gameplay). Each resize logged
  "lighting history copy off for 30 DLSS frames" and, for DLSS-G, "off for a swap chain
  resize" then "resources freed". No device loss. The "copy cancelled" guard didn't fire
  here, because the 30-frame hold already covered the double-rendered frame after the
  resize. It stays as a guard for fallbacks outside resizes.
- **The F11 hang was a DLSS SR bug, present since the lighting clamp (0.5.12), so also in
  1.0.0.** 1.1 should ship the fix. Frame generation's resize handling (DLSS-G off around
  the resize, resources freed, back buffer size from the swap chain) stays as well: it's
  what NVIDIA's guide asks for, even though it wasn't the cause.

### Results

| Question | Factory hook | Swap chain upgrade |
|---|---|---|
| Streamline loads (`slInit`, DLSS-G supported) | yes (fg.2) | not tested |
| Hook engages (game's swap chain through SL) | yes, via ReShade's call (fg.2; fg.1 missed it) | not tested |
| DLSS-G status ok | yes | not tested |
| Generated frames visible, 2x | yes: 78 -> 156 fps | not tested |
| Multi-frame 3x / 4x / Dynamic | 3x: 52 -> 156 fps; 4x: 39 -> 156 fps; Dynamic picks 2x (78 -> 156) | not tested |
| Pacing (below-SL sd / hitches) | real frames sd ~0.2 ms; output capped near 156 fps; below-SL not measurable in this order | not tested |
| Subtitles / QTE prompts | no visual issues (checked, run 3) | not tested |
| Crashes, device removed | F11 hung the GPU (runs 6-8; a DLSS SR lighting-copy bug, fixed in fg.9); none since | not tested |

### Blockers known before any run

- **Swap chain ownership:** covered above. The factory hook is the only way DLSS-G sees
  the creation it expects, and it puts ReShade under Streamline. The upgrade hook keeps
  ReShade on top but probably can't take over the buffers, if DLSS-G works on an upgraded
  swap chain at all.
- **UI:** no HUD-less or UI alpha, so subtitles and QTE prompts are interpolated, and in
  full-frame shots they'll smear or ghost when the scene moves under them. Letterbox-bar
  subtitles, if the game draws them in the bars, sit outside the tagged subrect and are
  copied unchanged. Producing HUD-less colour means finding the UMG/Slate pass and copying
  the scene before it: possible with the probe's pass tracking, but its own project.
- **Reflex placement:** the markers sit on the RHI thread around Present, not on the game
  thread. DLSS-G checks that Reflex is present (`eFailReflexNotDetectedAtRuntime`), but
  latency and pacing won't be as good as a native integration. The game thread runs one
  or two frames ahead of the RHI thread, and Reflex sleep can't hold it back from here.
- **Two NGX instances:** DLSS SR initialises NGX itself (dlss.cpp), and Streamline
  initialises it again for DLSS-G on the same device. Untested; watch the log for NGX
  errors.
- **Licensing and signing:**
  - The Streamline SDK and headers are MIT.
  - `nvngx_dlssg.dll` and the DLSS runtime are under the NVIDIA RTX SDKs license.
    Distribution is only allowed "incorporated in object code format into a software
    application", with "material additional functionality", and never "as a stand-alone
    product". **Decision for 1.1 (user):** the release package bundles them with the add-on,
    which is such an application, together with the five Streamline DLLs (MIT, and
    sl.reflex under the NVIDIA SDK license, which allows distributing its distributable
    parts inside an application). `build.ps1 -Package` builds it, with the licence texts
    in `licenses/` and an attribution in THIRD_PARTY_NOTICES.txt. All seven DLLs come from
    the Streamline 2.14.1 release and carry valid NVIDIA Corporation signatures;
    `nvngx_dlss.dll` is the same 310.9.1 as before. Remaining terms: attribute DLSS (done
    in the notices), distribute under consistent terms, and notify NVIDIA only before a
    commercial release (not the case for this free mod).
  - The guide says `Preferences::applicationId` is "provided by NVIDIA, required if using
    NGX components"; we pass only the project id and engine type, as DLSS SR does. Whether
    production `sl.dlss_g` accepts that is one of the things the first run shows.
  - Production builds verify signatures: we check `sl.interposer.dll`'s NVIDIA signature
    before loading it, and Streamline checks its plugins.
  - The development DLLs (on-screen DLSS-G debug text and hotkeys) are in the SDK's
    `bin\x64\development` if the production ones fail without saying why.

### Recommendation

- **Continue with Streamline, factory hook.** Streamline and ReShade coexist when
  Streamline's factory proxy takes ReShade's call to the real CreateSwapChain (game ->
  ReShade -> Streamline -> DXGI). DLSS-G then runs at 2x and 3x with status ok and even
  real-frame pacing, and the user saw no artifacts. The upgrade hook is untested and no
  longer needed.
- Next, in order of value:
  - switch DLSS-G off (or untag) while DLSS SR falls back, during loading and on cut
    frames;
  - try 4x and dynamic MFG on the RTX 5080;
  - check subtitles and QTE prompts deliberately in full-frame shots;
  - move the Reflex markers closer to the game thread;
  - a HUD-less copy only if UI artifacts show up.
- FSR 3 frame generation isn't needed for NVIDIA cards. It would only matter for
  non-RTX 40/50 users, and it would face the same swap chain ownership problem.

## Main TAA pass (Direct3D 11)

UE 4.26's standard compute TAA (`TemporalAA.usf`, `FTemporalAACS`), dispatched as
8x8 thread groups (480x200 for 3840x1600). Identify it by **position**, not by hash: it
runs right after the translucency-composite PS `04346c4e90a9704d`, and its output feeds
the bloom/eye-adaptation downsample (`ba3e1c97e7c22acb`) and the tonemapper
(`4b7e40416e1f163b`). Two permutations seen so far:

| Shader | When |
|---|---|
| `2f606fcaa114ea67` | 3840x2160 gameplay |
| `446e38a56d9893ac` | 1280x720 (the window briefly resized) |

Bindings (`2f606fcaa114ea67`):

| Slot | Texture | Meaning |
|---|---|---|
| t0 | 1x1 RGBA32F | eye adaptation (exposure) |
| t1 | 3840x2160 RGBA16F | **scene colour** (render res; output of `04346c4e90a9704d`) |
| t2 | 3840x2160 R32F view of R32G8X24 | **scene depth** (reversed-Z, UE4) |
| t3 | 3840x2160 RGBA16_UNORM | **velocity** (written by geometry; 4.26 packs depth into it) |
| t4 | 3840x2160 X32_G8 view of the depth | stencil (responsive AA) |
| t5 | 3840x2160 RGBA16F | **history** (its own output last frame) |
| u0 | 3840x2160 RGBA16F | **output**, read by the downsample and tonemapper |

Slots above t5 are probably stale bindings that the D3D11 RHI left in place (see Probe
issues).

## Other temporal passes

- `b569efd647217872` (CS, full res): has the same shape as TAA (eye adaptation, colour,
  depth, velocity, history), but runs much earlier, after the full-screen PS
  `cd75f114186bff1a` that reads the GBuffer. It's probably a second TemporalAA.usf
  permutation (SSR/SSGI denoise, or a mod: `NativeSSGI_P.pak` and
  `BetterReflections_P.pak` are installed). It's **not** the main TAA, because it runs
  before translucency and before the downsample.
- `5019389874611ed9` (CS, half res, R11G11B10F + RG8 history): looks like the
  SSAO/SSGI denoiser.
- `81326cac0579e50c` (PS, half res, RGBA16F + RG16F history): probably DOF or
  separate-translucency temporal.
- `885dce43623dfdaa` / `2a455ff261684512` (CS, 3D dispatch, 960x540): volumetric fog
  temporal reprojection.
- `6ab2395f4c09edf2` / `799ce21f5644c2ac` (buffers only): GPU particle simulation.

## Probe issues (all addressed in probe 0.2; waiting on a game run)

- D3D11 SRV slots stay bound after a pass, so every pass lists stale inputs. Decode the
  shader's own `dcl_resource` / `dcl_uav` declarations from its DXBC and list only the
  slots it declares.
- An output that the same shader wrote last frame through pooled ping-pong is
  mislabelled "HISTORY". That label should only apply to inputs.
- Jitter isn't captured yet. It's in the view uniform buffer (`View.TemporalAAJitter`)
  and in the TAA constant buffer. Dump the main TAA's constant buffers for a few frames.

## Next experiments

- Set `r.ScreenPercentage=67` and `r.TemporalAA.Upsampling=1` (Engine.ini
  `[SystemSettings]`) to check that the main TAA becomes the upscaler, with render-res
  inputs and an output-res result. DLSS has to replace exactly that pass.
- A `-dx12` run, to see if the D3D12 RHI starts.
- Repeat with the remaining `~mods` paks removed. `NativeSSGI_P.pak`,
  `BetterReflections_P.pak` and `zzz_NoCameraEffects_P.pak` all change post-processing.

### 1.0.0: first release (2026-10-16)

Same code as 0.6.1, with the version bumped. It ships as `QuarryDLSS.addon64` with the
README, LICENSE and THIRD_PARTY_NOTICES.txt. `nvngx_dlss.dll` isn't included.

### 1.1.0: frame generation, Ultra Quality and F11 fixes (2026-10-16)

The `fg-spike` branch as of 1.0.0-fg.9, merged into main, with the version set to 1.1.0.
Changes from 1.0.0 (details under "Frame Generation spike"):

- **DLSS Frame Generation** through Streamline 2.14.1: Off / 2x / 3x / 4x / Dynamic on the
  main panel, Off by default. Streamline is only loaded when it's on at launch.
- **Ultra Quality (77%) now runs DLSS.** It never did before: it uses TAAU permutation
  `2f606fcaa114ea67`, and NGX rejects its `UltraQuality` mode, so it's created as Quality.
- **F11 no longer hangs the GPU with DLSS on.** The stabilised-lighting copy is cancelled
  after any TAAU fallback and held off for 30 DLSS frames after a swap chain resize.
- The release zip bundles the NVIDIA runtime: `nvngx_dlss.dll`, `nvngx_dlssg.dll` and the
  five Streamline DLLs, with their licence texts (`build.ps1 -Package`).

### 1.1.1: colour-format crash fix (2026-10-22)

- A user report showed UE's "AllocateTexture ... DXGI_ERROR_DEVICE_REMOVED" dialog. That's the
  same signature as DarkPicturesDLSS 0.1.0 on The Devil in Me: the lighting clamp pass made an
  RGBA16F SRV of the game's scene colour regardless of its format. On an R11G11B10F scene
  colour (`r.SceneColorFormat` lowered by settings or mods), that view is invalid and D3D12
  removes the device (INVALID_CALL) on the first DLSS frame. QuarryDLSS 1.0.0 and 1.1.0 have
  this bug. 1.0.0 also has the F11 resize hang (fixed in 1.1.0).
- 1.1.1 is v1.1.0 plus the shared fix from the `dark-pictures` branch (d8cecf4):
  - colour, velocity and depth views use the texture's own format;
  - unsupported formats are refused with a status line;
  - the game's input formats are logged once per change.
- "Stabilise screen-space lighting" is off by default (as in DarkPicturesDLSS 0.1.10). It
  only matters with the SSGI/SSR pak mods, and it's where both device-removal bugs were. Its
  ReShade.ini key changed to `StabiliseScreenSpaceLighting`: 1.0/1.1 wrote the old default (on)
  with every settings change, so the old key can't tell a choice from the default.
- Not tested in game with an R11G11B10F scene colour on The Quarry. The same code runs on
  The Devil in Me (R11G11B10F) without a crash.
