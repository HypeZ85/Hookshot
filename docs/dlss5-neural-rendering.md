# Experimental DLSS 5 Neural Rendering

This adds optional single-pass Neural Rendering (NR) to the existing direct
Vulkan NGX integration. It runs on the scene color before DLSS SR/DLAA and
before Hookshot's HUD/menu composition. It does not add Streamline or another
injection layer. Ordinary rendering continues when NR is unavailable.

## Requirements and availability

Build with the existing `HOOKSHOT_NGX_SDK` workflow, select NVIDIA DLSS or
DLAA under Graphics > Upscaler, and supply your own compatible NVIDIA
`nvngx_dlssnr.dll` in the game's `ngx` folder on Windows. This repository and
its build scripts do not include or download that DLL. A compatible GPU,
driver and runtime are required; SR support alone is insufficient.

NR uses community-documented feature 18 (`Reserved18` in the public SDK),
not a public NVIDIA NR SDK contract. Setter success is not proof that a
parameter changes the image. Validation used NGX SDK/runtime 310.9.16 with
an RTX 5080 on Windows; results must not be generalized to other runtimes.
Native Linux support is capability-gated and experimental; see
[the Linux notes](dlss5-linux.md).

Missing/invalid DLLs, unsupported hardware and recoverable NGX failures are
reported only inside the DLSS 5 tab, in both Base and Advanced. They do not
open a startup popup or erase saved preferences. Retry detection refreshes
the cached check; changing driver/runtime initialization may require restart.
Native HDR is bypassed because this NR path consumes SDR scene resources;
SDR and the existing Expanded HDR presentation path remain available.

## Controls

The DLSS 5 tab follows the existing ImGui menu drawing/input conventions.
Base exposes requested/effective enable state, intensity, visual profiles,
Compare, shortcut mode, toolbar position and reset. Advanced has two groups:
General (history, masks and diagnostics) and Quality (model and color controls).
Both choices are always visible in the existing two-cell selector, with the
active group highlighted. Click its label or select with Left/Right on a
keyboard or controller; selecting the active group does not switch away.
Both edit one state. Old Rendering, Masks and Diagnostics selections migrate
to General; a saved Quality selection remains Quality.

| Setting | Implementation |
| --- | --- |
| Intensity, Style, Model preset | Native NGX creation parameters `DLSSNR.Intensity`, `DLSSNR.Style`, `DLSSNR.Hint.Render.Preset` |
| Local structure/tone, Skin structure | Native `DLSSNR.LocalStructureStrength`, `DLSSNR.LocalToneStrength`, `DLSSNR.SkinStructureStrength`; negative skin sentinel follows structure |
| Auto skin mask, UI correction | Native `DLSSNR.UseAutoMask`, `DLSSNR.UICorrection` flags; the private skin mask is not exposed for visualization |
| Color strength | Compute composition in linear SDR Rec.709: mix original/NR chromaticity at NR luminance, preserve input alpha; independent of global intensity |
| Temporal history | Auto uses NR history; Disabled sends the native reset flag every evaluation for diagnosis |
| Debug view | Actual original, raw NR, absolute RGB difference x20, device depth or motion resources; diagnostic outputs bypass NR processing |
| Statistics | Extends the existing top-right FPS counter with frametime, effective NR dimensions and nonblocking Vulkan GPU timestamps around NR evaluation; unavailable measurements are `n/a`. Uses one shared panel, also when the separate FPS option is off. |

Legacy defaults are retained: intensity 35%, structure/tone/skin 100%,
preset 0, style 0, auto mask/UI correction on, color strength 100%.
Original disables NR. Faithful selects style 1 and original chromaticity,
with the other legacy values; Balanced keeps the legacy defaults; Intense sets intensity to
100%, without claiming greater accuracy. Manual model/color edits select
Custom. Runtime tests distinguished preset 1 from default; legacy hints 2/3
are loaded but not offered as new choices because they matched default.

Unsupported controls are intentionally absent: Global Tone showed no image
effect with the tested runtime; render scale needs a validated resampling
contract for color/depth/motion/history; repeated NR evaluations need a
validated temporal contract; detail stabilization needs reliable additional
temporal reconstruction; semantic object masks need renderer categories
that are not available. No simulated versions or guessed segmentation are
presented as working settings.

## Compare

Select Compare and press Right (or click). The full panel is replaced by a
small translucent top/bottom toolbar; the viewport and central scene remain
unchanged. F8 toggles NR or temporarily shows the original while held,
according to shortcut mode. The controller equivalent is displayed on the
toolbar and avoids the currently bound confirm/back face buttons. The menu
continues to consume input so comparison buttons do not also trigger game
actions. Selecting Toolbar position in Base shows an inert placement preview.

The toolbar has NR, intensity, profile, Save and Cancel. Both exit to the
previous settings tab; Save commits the comparison changes, Cancel restores
the entry snapshot. Temporary shortcut presses do not write the config.
Disabling NR skips its evaluation while retaining resources for a fast return;
re-enabling resets the relevant temporal history.

## Resource and configuration handling

Old `dlss5*` keys remain readable and are clamped on load. New profile/history/
diagnostic/comparison values are persisted with the existing settings. Model
edits are coalesced after slider release and a quiet period, with a minimum
interval/evaluation count between recreations. Color composition, debug and
history reset are dynamic. Feature replacement keeps the last working model
until a candidate succeeds; recoverable failure falls back and is logged once
per state transition. NR shares the existing deferred Vulkan resource lifetime,
resize, NGX session and device-loss handling.

## Validation and limitations

CPU tests cover activation policy, bounds/coalescing, profiles, Toggle/Hold,
comparison Save/Cancel/persistence and actual menu overlay ordering. Run the
complete game-free suite with `python -I -B tests/run_all.py`. The optional
`tests/check_dlss_nr_linux.sh` compiles native Linux objects/CPU checks, without
claiming a full Linux link or GPU test.

Earlier local Windows GPU tests exercised native parameter A/B comparisons,
color composition, actual diagnostic buffers, repeated comparison toggles,
resize and missing-DLL fallback. The current ImGui menu was reviewed live on
Windows. Group migration, two-cell selection and the shared overlay ordering
also have game-free CPU coverage. The latest layout refinements were compiled;
this does not claim additional visual/controller GPU testing for those changes.
Native Linux NR and Native HDR NR are not verified or claimed.
One earlier Windows exit coincided with an NVIDIA TDR/LiveKernelEvent 141;
its cause remains unresolved and a clean shutdown must not be assumed from
forced process termination in a test harness.
