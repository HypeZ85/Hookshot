# Experimental DLSS 5 Neural Rendering

This adds optional single-pass Neural Rendering (NR) to the existing direct
Vulkan NGX integration. The experimental Stage setting runs it on the scene color
before or after the selected temporal upscaler, always before the game HUD and
Hookshot's menu composition. It does not add Streamline or another
injection layer. Ordinary rendering continues when NR is unavailable.

## Menu translations

NR labels, profiles, tooltips, availability errors and Compare controls are covered
by the existing menu translation tables for all 13 non-English locales. English
remains the source language and fallback. Runtime identifiers, DLL filenames,
environment switches and numeric formats remain literal. Compare translates its
component labels before composition, and fitting clips translated UTF-8 text.
Language selection uses the existing Menu language setting; saved NR keys and
values are unchanged. No translation service or network access is used at runtime.

The initial translations were machine-assisted, with Italian review and technical
terminology corrections across locales. Native-speaker review remains welcome,
as for the upstream tables. Coverage and runtime-lookup tests check every locale,
including percentages, composed labels and unavailable-runtime messages. Visual
fit and glyph coverage across every platform/aspect ratio have not been verified.

## Requirements and availability

Build with the existing `HOOKSHOT_NGX_SDK` workflow, select DLSS, DLAA or FSR on the shared temporal
chain under Graphics > Upscaler, and supply your own compatible NVIDIA
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
reported only inside the Graphics > DLSS 5 Neural Rendering submenu, in both Base and Advanced. They do not
open a startup popup or erase saved preferences. Retry detection refreshes
the cached check; changing driver/runtime initialization may require restart.
Native HDR is bypassed because this NR path consumes SDR scene resources;
SDR and the existing Expanded HDR presentation path remain available.

## Controls

The Graphics > DLSS 5 Neural Rendering submenu follows the existing ImGui menu drawing/input conventions.
All settings are on one scrollable page, with standard non-selectable headings:
Base Options (enable state, visual profile, style, intensity), Advanced Options
(model, local structure/tone, skin structure, color and stage), Masks, Compare
and Diagnostics. Visual profile precedes Style; manual Style edits select Custom.
There are no page/group selectors. Old `dlss5_advanced`, `dlss5_group` and
`dlss5_compare_mode` keys are ignored and omitted on the next save, retaining
all actual NR tuning preferences.

| Setting | Implementation |
| --- | --- |
| Stage | Renderer routing before/after the temporal engine. Before is the legacy default. After runs at the engine output resolution; real depth/motion are resampled, motion scaled into output pixels and reversed depth normalized for NR |
| Intensity, Style, Model preset | Native NGX creation parameters `DLSSNR.Intensity`, `DLSSNR.Style`, `DLSSNR.Hint.Render.Preset` |
| Local structure/tone, Skin structure | Native `DLSSNR.LocalStructureStrength`, `DLSSNR.LocalToneStrength`, `DLSSNR.SkinStructureStrength`; negative skin sentinel follows structure |
| Auto skin mask, UI correction | Native `DLSSNR.UseAutoMask`, `DLSSNR.UICorrection` flags; the private skin mask is not exposed for visualization |
| Color strength | Compute composition in linear SDR Rec.709: mix original/NR chromaticity at NR luminance, preserve input alpha; independent of global intensity |
| Temporal history | Auto sends NR Reset while the SR input is jittered, and on cuts/resizes/reactivation; Disabled always sends Reset. SR retains its own jitter/history |
| Debug view | Actual original, raw NR, absolute RGB difference x20, device depth or motion resources; diagnostic outputs bypass NR processing |

The upstream FPS counter is unchanged. The former NR Statistics option and its
GPU timestamp collection have been removed; legacy `dlss5_stats` entries are
ignored and retired on the next settings save without changing FPS preferences.

Legacy defaults are retained: intensity 35%, structure/tone/skin 100%,
preset 0, style 0, auto mask/UI correction on, color strength 100%.
Original disables NR. Faithful selects style 1 and original chromaticity,
with the other legacy values; Balanced keeps the legacy defaults; Intense sets intensity to
100%, without claiming greater accuracy. Manual model/color edits select
Custom. In runtime 310.9.16, Model 1 is the earlier model and Model 2 is the
October update selected by default. The menu labels saved preset 0 (runtime
default) and explicit preset 2 as Model 2; preset 1 as Model 1. Numeric saved
values remain unchanged; hint 3 remains loadable but is not offered for new
selections. Other runtime versions may use a different default. NVIDIA describes
October's model as faster/smaller with comparable fidelity, not a guaranteed
quality loss in every scene. See [NVIDIA's announcement](https://www.nvidia.com/en-us/geforce/news/dlss-5-faster-ai-model-hogwarts-legacy-rtx-spark-laptops/)
and [the original runtime verification report](https://raw.githubusercontent.com/mauri870/DLSSNR-RDNA3/rdna3/docs/ngx-verification/310.9.16/NGX-VERIFICATION.md),
which identifies default/preset 2 as the new model and preset 1 as the old model.
These external timings are not Hookshot performance measurements.

Unsupported controls are intentionally absent: Global Tone showed no image
effect with the tested runtime; render scale needs a validated resampling
contract for color/depth/motion/history; repeated NR evaluations need a
validated temporal contract; detail stabilization needs reliable additional
temporal reconstruction; semantic object masks need renderer categories
that are not available. No simulated versions or guessed segmentation are
presented as working settings.

## Compare

Select Compare and press the normal confirm button, Right, or click. The full panel is replaced by a
small translucent top/bottom toolbar; the viewport and central scene remain
unchanged. Its NR control toggles the comparison through mouse, keyboard or
controller navigation. There is no dedicated F8 or controller-face shortcut,
no Hold mode and no shortcut preference. F8 is not polled, consumed or included
in the comparison-release gate. The menu
continues to consume input so comparison buttons do not also trigger game
actions. Compare Toolbar position chooses the top or bottom edge; the toolbar
is shown only while Compare is active. Retry detection and Reset NR settings
also accept confirm, Right, or a mouse click.

The toolbar has NR, intensity, profile, Advanced/Base, Save and Cancel. Advanced
adds two compact rows at the same screen edge: a parameter carousel followed by
its value or draggable slider. Up/Down moves between the toolbar, parameter and
value rows. Left/Right browses parameters on the second row and adjusts values
on the third. Dimmed neighboring names show that the parameter list scrolls;
the mouse wheel over the name also browses it. Clicking either half of a discrete
value decreases/increases it; percent sliders support click and drag. All
graphic settings remain accessible; the repeated enable switch is excluded because
the toolbar already has an NR toggle. Retry detection
and Reset NR settings stay in the full menu only. Runtime availability locks
are shared with the full menu.
Switching back to Base retains the preview edits. No viewport resize is used.

Save and Cancel both exit to the
previous NR settings page; Save commits the comparison changes, Cancel restores
the entry snapshot, including temporal history, stage and toolbar position.
Debug views remain session-only and return to the entry view on exit. Temporary
toggles and advanced edits do not write the config before Save.
Disabling NR skips its evaluation while retaining resources during Compare
for a fast return. Outside Compare, two seconds Off release the model, images,
and composition pipeline through deferred destruction. Re-enabling
resets the relevant histories once after NR actually evaluates successfully.
Availability failure and Native HDR do not repeatedly reset SR.

## Resource and configuration handling

Old `dlss5*` keys remain readable and are clamped on load. Profile, history and
comparison values use the existing persistence system. Debug view is session-only;
legacy `dlss5_debug` values are ignored and removed on the next save. Model edits are coalesced after slider release and a quiet period, with a minimum
interval/evaluation count between recreations. Color composition, debug and
history reset are dynamic. Feature replacement keeps the last working model
until a candidate succeeds; recoverable failure falls back and is logged once
per state transition. NR shares the existing deferred Vulkan resource lifetime,
resize, NGX session and device-loss handling.

## Validation and limitations

CPU tests cover activation policy, bounds/coalescing, profiles, toolbar toggles,
comparison Save/Cancel/persistence and actual menu overlay ordering. Run the
complete game-free suite with `python -I -B tests/run_all.py`. The optional
`tests/check_dlss_nr_linux.sh` compiles native Linux objects/CPU checks, without
claiming a full Linux link or GPU test.

Earlier local Windows GPU tests exercised native parameter A/B comparisons,
color composition, actual diagnostic buffers, repeated comparison toggles,
resize and missing-DLL fallback. The current ImGui menu was reviewed live on
Windows. Section ordering, one-page visibility and the shared overlay ordering
also have game-free CPU coverage. The latest layout refinements were compiled;
this does not claim additional visual/controller GPU testing for those changes.
Native Linux NR and Native HDR NR are not verified or claimed.
One earlier Windows exit coincided with an NVIDIA TDR/LiveKernelEvent 141;
its cause remains unresolved and a clean shutdown must not be assumed from
forced process termination in a test harness.

## Maintainer review follow-up

The NR page is now a Graphics submenu directly below DLSS model, using the same
navigation mechanism as Controls > Motion. It is absent from the main tab strip
and hidden in builds without NGX. The sectioned page and Compare share the same settings. The experimental branch also allows FSR engines on the shared temporal chain to
consume NR. This means independence from the selected upscaler, not from NVIDIA:
the existing NGX session still requires compatible NVIDIA hardware, driver and
the SR runtime to initialize, even when FSR is selected. FSR's separate legacy
path (`RX_VK_FSR3_CHAIN=0`), the game's own upscaler and Upscaler Off do not run NR.
There is no final-present fallback: menus and the game HUD are never NR inputs.

NR allocates at the maximum input-image extent for Before, or the temporal
engine's output extent for After, with the
actual frame dimensions in all color/depth/motion/output subrects. Dynamic
resolution within that allocation no longer recreates the feature or images.
NR history is reset on a subrect size change; that reset does not erase SR history.

Both opaque NR CreateFeature/EvaluateFeature calls drain the upstream command
recorder with `rec::boundary(rec::B_DIRECT)`. The recorder test recognizes the
native NR API calls and checks that removing either boundary is caught.

### Jitter contract and limits

No verified NR jitter-offset, exposure or camera parameter is present in the
community contract used here, or in the inspected 310.9.16 runtime's parameter
strings. In particular, setting a guessed `DLSSNR.JitterOffset*` would not prove
that NGX consumes it. This path therefore sends NR Reset for every jittered
input frame. It leaves SR jitter, motion vectors and SR history intact. This
avoids NR accumulating misregistered temporal samples, but is not a claim of
temporally stable NR output or a validated unjittered reconstruction. Temporal
History's help describes this restriction. An unjittered NR path, with matched
depth/motion and a validated return into jittered SR coordinates, remains future
renderer work; a simple color-only resample would not establish that contract.

### Experimental API and fault injection

Feature 18 / `Reserved18` and the `DLSSNR.*` keys remain a community-documented,
undocumented-by-NVIDIA contract. Driver/runtime updates can change it. The user
supplies the proprietary Windows runtime; no SDK, runtime or download is added.
Linux has no known native NR runtime and currently provides opt-in capability
probing and fallback only. Windows defaults to Off, with explicit menu opt-in;
requiring an additional environment opt-in before shipping is a maintainer
policy decision, not an implication of this implementation.

`RX_VK_NR_TEST_REJECT_INTENSITY` is read only when a developer compiles
`vk_dlss.cpp` with `HOOKSHOT_NR_TESTING`. Normal builds do not read this fault
injection variable. It deliberately rejects creation at a chosen intensity to
exercise candidate rollback without exhausting GPU memory.

### Subrect GPU probe

`tests/dlss_nr_subrect_probe.cpp` is an optional Windows-only, synthetic-input
NGX probe. It writes no image files and is not run in game-free CI. Supply your
own NGX SDK and NR runtime. From a Visual Studio developer prompt, with SDK
and Vulkan include paths available:

```bat
cl /std:c++20 /EHsc /MT /I "%HOOKSHOT_NGX_SDK%\include" /I "%VULKAN_SDK%\Include" tests\dlss_nr_subrect_probe.cpp "%HOOKSHOT_NGX_SDK%\lib\nvsdk_ngx_s.lib" advapi32.lib user32.lib /Fe:build\dlss_nr_subrect_probe.exe
build\dlss_nr_subrect_probe.exe "C:\path\to\your\runtime" "C:\path\to\writable\ngx-cache"
```

On an RTX 5080 with runtime 310.9.16, one 640x360 feature evaluated
640x360 -> 512x288 -> 432x240 -> 640x360 successfully. Readback found no writes
outside the active output subrect and no NaN/Inf. The final full-size output
matched the first bit for bit. This verifies the tested runtime's changing
subrect handling, not every resolution, driver, or temporal image quality.

## Environment overrides

These optional overrides last for the current run; saves keep the player
preferences rather than the override. None selects the upstream upscaler:
choose DLSS or DLAA in Graphics first.

| Variable | Behavior |
| --- | --- |
| `RX_VK_DLSS_NR` | Override the NR enable request (`0`/`off` disables, `1` enables); availability checks still apply. |
| `RX_VK_DLSS_NR_DEFAULT` | Set the initial request only when no saved NR enable preference exists; an explicit `RX_VK_DLSS_NR` takes precedence. |
| `RX_VK_DLSS_NR_INTENSITY` | Override intensity as a 0–2 multiplier (1 = 100%), clamped to that range; nonfinite input is ignored. |

The unrelated `RX_VK_DLSS_START` helper has been removed. Periodic availability
checks use a nonblocking lock and retain the last diagnostic while NR is busy;
explicit Retry still performs a fresh check.

## Experimental stage validation

After receives the reconstructed image, with depth and motion resampled by
`dlss_nr_inputs.comp`. It supplies zero color jitter and permits NR history in
Auto. Nearest scene samples are used, not geometry-aware high-resolution
reconstruction: edge/disocclusion quality needs live evaluation, especially with
lower render scales. Stage is snapshotted once per frame so a concurrent UI edit cannot evaluate NR
both before and after in the same frame. Stage changes reset histories and release the old NR feature
through deferred destruction. Repeated toggles retain it while Compare is active.
Post-stage diagnostic composition uses a separate destination so it never samples
and writes the same image. Native HDR remains unavailable.

`tests/test_dlss_nr_stage.py` executes the production adapter with fake Vulkan/NGX
calls. It checks both resolutions, real input selection, depth orientation,
motion-resampling push constants, NR Off avoiding all preprocessing, Compare
overrides, cached availability failure, allocation/evaluation fallback and debug
routing. The shader regeneration check covers the new SPIR-V header. The menu
transaction test also restores Stage on Cancel. These game-free checks do not
establish visual quality or GPU timing for the new Before/After paths. In
particular, After at a larger resolution can cost substantially more than Before.

Local validation on 2026-11-12: Windows NGX renderer and matching full API-21 game
build succeeded. All 12 targeted NR/menu/shader checks passed. A local Vulkan
probe on RTX 5080 ran the actual new shader with synthetic depth and signed
motion inputs: every pixel matched at 1x and 2x scale, with normal and reversed
depth (four cases). This verifies resampling arithmetic, not NR image quality,
FSR/NR visual integration or GPU timings. The live game was opened for manual
review; automated gameplay input and screen captures were not used.

The final Windows game-free suite completed with 499 passed, 36 skipped and one
failing check (`nx2/test_nup_paths.py`): its symlink fixtures require a Windows
privilege unavailable here (WinError 1314), the same failure as on the parent
branch. The subsequent Stage persistence test also passed with explicit invalid
file values, clamping, Cancel restoration, Save publication and INI output.
Linux compilation/runtime, other GPU families, real controller input and the new
stage's visual quality remain unverified. The existing PR is unchanged.
