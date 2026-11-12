# DLSS 5 review follow-up validation

This follow-up integrates the feature with upstream main through
`fc375e9612ae528e9ec62dd98bc4a4578f84bb67` (10 November 2026). The shared
`vk_temporal` chain, FSR engines, recorder, Audio tab and current Debug layout
are retained. NR remains part of the DLSS engine alone.

## Review changes

| Review point | Change | Evidence |
| --- | --- | --- |
| Dynamic resolution recreation | NR creation and allocations use the SR maximum input size; each evaluation supplies the current subrect | Real synthetic GPU subrect probe; Windows engine build |
| Native HDR resets SR every frame | Reset only when leaving an active NR path | Production early-exit policy executed over 600 blocked frames |
| Unavailable NR resets SR every second | Availability checks preserve SR history after the initial transition and avoid repeated destruction | Production policy executed with a fake clock and repeated failed probes |
| Off retains model VRAM | Keep it for Compare, release after two seconds Off outside Compare using the shared deferred lifetime | Clock-driven grace/re-enable/Compare tests |
| Recorder ordering | Drain B_DIRECT before both native NGX NR calls | Source validation and negative mutations that remove the boundaries |
| Jitter metadata | No verified NR keys found; reset NR history for jittered inputs and preserve SR jitter/history | Contract inspection and code; temporal visual quality remains unverified |
| Production fault injection | Requires HOOKSHOT_NR_TESTING; ordinary builds do not read the variable | Windows build and source inspection |
| Always-visible DLSS 5 tab | Graphics submenu under Upscaler, hidden without NGX | Executed production row visibility and parent/submenu navigation |
| Compiler discovery | Four original NR tests and the lifecycle test declare RUN_ALL_ARGS | Full test runner uses its detected compiler; main has replaced the old ARGS table |
| Shader header coverage | Composite shader added to test_dlss_actor.py | glslang shader-to-header comparison passed |
| Debug persisted into next launch | Ignore legacy dlss5_debug on load and omit it on save | Executed production ini load/save loops; legacy debug=5 did not bypass NR in the live renderer |

## Builds and automated checks

- Windows NGX Vulkan renderer: linked successfully with llvm-mingw 23 and the
  local NGX SDK. No SDK files or binaries are part of the change.
- Complete isolated Windows game: built successfully, including the current
  host/renderer API, in 16 minutes 27 seconds. The installed renderer is the
  final separately checked NR build.
- Windows no-SDK: vk_dlss.cpp compiles without HOOKSHOT_NGX.
- Native Linux (offline Debian 12 / GCC 12.2): vk_menu.cpp, vk_imgui.cpp and
  vk_dlss.cpp with NGX, plus vk_dlss.cpp without NGX, compile; four native CPU
  checks pass. This is not a Linux renderer link or GPU run.
- Full game-free Windows suite: **323 passed, 6 failed, 21 skipped**. All five
  NR checks, shader header comparison, affected menu fixtures and FPS ordering
  checks passed. The six failures were also reproduced on a separate clean
  upstream-main checkout with the same tools:
  - nx2/test_nup_paths.py: Windows link/junction privileges and sandbox limits.
  - test_bake_ds.py: zstd command-line tool unavailable.
  - test_builder_details_hitbox.py, test_builder_dlss_update.py and
    test_updater_asset_digest.py: .NET runtime present, .NET SDK absent.
  - test_recorder_sync.py: its Windows negative exit fixture also exits cleanly
    with the intentionally old recorder destructor, so the mutation is not
    caught on this local toolchain. Its NR boundary/source checks and normal
    command-stream runs passed before that unchanged negative fixture failed.

The runner's Windows result is not reported as an all-green suite or a CI
result. No unrelated tests were weakened to hide these failures.

## Real GPU checks

Hardware: NVIDIA GeForce RTX 5080, driver 617.42, user-supplied NR 310.9.16.
The optional tests/dlss_nr_subrect_probe.cpp uses synthetic gradients and
constant depth/motion; it writes no images. It was compiled with both
llvm-mingw and MSVC. One 640x360 feature evaluated 640x360, 512x288, 432x240,
then 640x360 without recreation. GPU readback found no writes outside each
active subrect and no NaN/Inf; the first and last full-size outputs matched
bit for bit. This is evidence for that runtime and those sizes only.

The headless game exercised over 5,400 actual NR evaluations with the upstream
threaded recorder, at an active size of 1440x810. It logged no NR evaluation
or device-loss failure. The headless path does not normally tick menu input,
so that run is not counted as a Toggle/Hold, menu or normal-window-exit test.
It was stopped by the test harness.

The normal-window test then exercised both missing-runtime and available-runtime
starts with RX_VK_RECORDER=thread, without image capture. The missing-runtime
run rendered normally, logged the missing nvngx_dlssnr.dll, and produced no
native modal dialog. After restoring the DLL and restarting, preflight reported
available=1 and NR evaluated successfully. A legacy dlss5_debug=5 entry was
ignored: NR ran normally, and a subsequent settings save omitted that key.

Scripted menu input entered Graphics > DLSS 5 > Compare with Right, toggled NR
four times, cancelled back to settings, selected Hold, held/released the shortcut
and cancelled again. Evaluation counters stayed unchanged while NR was bypassed.
No feature was recreated during these comparison transitions. Ordinary Off then
released NR resources after two seconds; switching On created one new feature.
Both normal-window runs exited after WM_CLOSE to their own Hookshot window,
within the 30-second watchdog and without forced termination. This is not a
claim that the earlier intermittent shutdown/TDR cause has been fixed.

The test used a separate copy of the saves. The DLL and original options were
restored afterwards; the original and restored options have identical SHA-256.
The contributor's main game installation was not changed.

## Remaining validation limits

No screenshots or generated previews were taken. The contributor reviewed and approved the updated menu in the running game
before authorizing publication of this follow-up. Physical controller navigation, every aspect ratio,
all DRS ranges, injected device loss and temporal NR image quality have not
been exhaustively verified. The earlier Windows exit/TDR report remains an
open limitation; this follow-up does not claim a general shutdown fix.
Native Linux NR has no known compatible runtime and remains capability
probing/fallback, not verified Linux GPU support.

## Main integration on 11 November 2026

Integrated 144 upstream commits through
`2ed98c46843b37a45b9afec531aba5abcd579e1a`, preserving the new direct NGX
Frame Generation, HDR SR, rounded render ranges, runtime control bridge,
environment overrides and FPS-counter levels/graph/corner selection.

The NR Statistics option and its GPU timestamp queries have been removed.
The FPS counter keeps upstream's rendering, levels and selected corner; old
`dlss5_stats` preferences are ignored and removed on the next save.
The control bridge has a DLSS 5 submenu name;
NR model drags publish an atomic flag for the render thread. NGX initialization
is serialized for the background probe, SR/NR and the new FG caller. Native
HDR bypasses the SDR-only NR/composite path without disturbing SR history.
Temporary environment values remain temporary when settings are saved.

Windows NGX renderer and complete isolated game built successfully (the game
build took 5:03, including 1:56 compilation with existing local object caches).
The no-NGX Windows DLSS object also compiled. The full game-free suite ended
with **377 passed, 5 failed, 30 skipped**. NR, affected menu/environment,
control bridge, HDR and shader checks passed. Remaining failures were the
three .NET SDK-dependent checks, Windows link-privilege extraction fixtures,
and the previously observed recorder negative exit fixture. No assertions
were removed to hide these failures.

The Linux check could not run: Docker's Linux engine pipe was unavailable,
including after attempting to start the installed Docker Desktop. Previous
Linux object checks above apply to the earlier revision, not this merge.
The contributor approved the menu in the running game, then requested removal
of Statistics before publication. No screenshots or generated previews are used.

Four further upstream commits through `d8f4a173dafb3dc20553145e37d6dbf46dccc91b`
were integrated before publication, retaining the new reactive mask, VRAM
budget row and DLSS size diagnostics. The NR settings migration coexists with
the new memory-budget migration. The reactive mask still feeds SR independently
of NR; it is not presented as a semantic NR control mask.

Final Windows NGX renderer link and no-SDK DLSS object compile passed after
Statistics removal. The complete suite passed **379 checks, with 6 failures
and 30 skips**. NR, FPS overlay ordering, reactive-mask and affected menu
checks passed, including loading the retired `dlss5_stats=1` preference
without enabling FPS or changing NR. The additional failure is the new
`test_vram_budget_row.py` fixture's unused `kVramMinimumOnly` constant under
llvm-mingw clang 23 with `-Werror`; it reproduces identically from unmodified
upstream `d8f4a173` sources. The other five failures have the causes listed
above. No test assertions were weakened. The rebuilt renderer was staged in
the isolated game folder; this last removal has not had another GPU play test.

## Follow-up review and compact-menu merge

Integrated main through `6c8c1bfb08f34a7f9742b768001480d9710664d2`, including
its compact tuning bar, texture mip bias and recorder test fix. NR keeps a
separate Compare toolbar and retains General/Quality selection in the shared
row-value renderer. Full-menu navigation and the upstream compact-mode state
are preserved; the placement option does not display or resize a toolbar.

Compare, Retry detection and Reset NR settings accept normal confirm or Right
as well as mouse input. Simultaneous confirm/Right executes once. The setting
is named Compare Toolbar position. Merge-corrupted Inventory/Teleport UTF-8
strings and the NGX path comment were restored from upstream. RX_VK_DLSS_START
was removed and NR environment overrides documented. The periodic capability
probe uses try_lock and returns the cached issue while NR is busy; explicit
Retry still checks synchronously. The actions regression executes production
branches and tests lock contention from another thread without a GPU.

Windows NGX renderer linked and the no-SDK DLSS object compiled. The rebuilt
renderer was staged in the isolated game directory while the game was closed.
No fresh GPU play test, screenshot or Linux build was performed in this update.

Final complete suite: **383 passed, 5 failed, 30 skipped** in 96 seconds. NR
actions/menu, FPS ordering, compact tuning and recorder checks pass. Remaining
failures are the three missing .NET SDK fixtures, Windows link privileges and
the upstream VRAM-budget fixture unused-constant warning documented above.


## Maintainer follow-up on 12 November 2026

Merged 73 upstream commits through `fce53453a2fdd701bb466c4e2c0de839bd6f2042`,
retaining the new Graphics sections, native HDR tone mapper, field-of-view
control, secondary controller bindings and renderer API version 18. The
Graphics entry and compact-page title now read DLSS 5 Neural Rendering. Its
entry is immediately after DLSS model, within the upstream scaling section.

The keyboard regression now ends the shortcut-routing extraction before the
periodic NR probe, supplies the NR state/persistence stubs, and exercises F1
cancelling Compare for both queued and polled input. Existing shortcut and
saved-configuration assertions remain intact. The NR menu check verifies the
new label and ordering; Graphics-section and affected menu tests pass.

Windows NGX renderer link and no-SDK DLSS object compile passed. Linux and a
new live NR GPU evaluation have not been verified for this merge. The menu
conversion is prepared for contributor review in the running game, in
accordance with their earlier request to approve official menu conversions
before publication. No screenshots or generated previews were captured.


The complete Windows suite finished with **433 passed, 9 failed, 34 skipped**
(106 seconds, eight concurrent checks). Menu keys, NR, Graphics sections,
audio/stick/framegen, recorder and FPS checks pass. Six failures require the
missing .NET SDK (including new portable-builder checks); extraction-path
fixtures require Windows link privileges. The remaining two are upstream's
unused `kVramMinimumOnly` fixture constant under clang 23 / Werror and the new
`HOS_FPS_LOG` set-means-on check in `host/oot_high_fps.c`, which also exists
unchanged on upstream `fce53453`. Assertions were not weakened to hide them.

The complete isolated Windows game build passed in 15:11, with 650 compilation
units and zero compilation failures. The SDK-enabled build compiled and staged
its own API-18 renderer (current upstream ignores the prebuilt override when
building with NGX). Intermediate build outputs were cleaned by the builder.
The normal-window live review starts with NGX initialized and NR preflight
available. Shader preparation is in progress; this is not a fresh NR quality
or physical-input validation claim. Publication remains held for the
contributor's explicit approval after trying the game.


## Publication after live approval

The contributor approved the renamed/repositioned menu in the running game.
Before publication, four more upstream commits through
`9df055f23cd4604a6f520631a6dd4154a55a22b4` were integrated without conflicts.
They change Reflex submit tagging, RADV fallback/optimization and the sound
clock, and do not change the approved menu or renderer API. Final Windows
NGX renderer link and no-SDK DLSS object compilation pass. The complete suite
now reports **435 passed, 9 failed, 34 skipped** (105 seconds). All NR/menu,
Graphics sections, recorder, FPS, Reflex and high-FPS checks pass. The nine
failures retain the environmental/upstream causes listed above. The full-game
build and live approval precede only these last four non-menu upstream commits;
no fresh Linux or GPU-quality validation is claimed for this final merge.


## Follow-up merge through c544828f

Integrated 59 upstream commits through
`c544828ff2d545829505cd894bb7968c7c2c5fcb`. Resolved the menu persistence and
stick-dead-zone fixture conflicts by retaining both game/menu language state
and NR settings. Game language remains owned by language.txt; NR debug stays
session-only, and comparison saves keep their transactional masking. The NR
menu regression now loads a legacy game-language key without changing that
choice and verifies it is absent from the options save.

Retained upstream's menu localization and keyboard prompt infrastructure,
bug-report action, microphone recovery and DLSS SR/Frame Generation failure
recovery. The NR page's layout/position and actions are unchanged. NR strings
not present in the upstream translation tables use their documented English
fallback. No translation or runtime capability is simulated.

Windows NGX renderer link and no-SDK DLSS object compilation pass. The complete
suite reports **455 passed, 8 failed, 34 skipped** (114 seconds). NR, menu/input,
localization, DLSS checks, DLSS-G runtime and recorder checks pass. The extra
language/NR persistence regression also passes. Remaining failures are six
missing .NET SDK fixtures, Windows extraction-link privileges and the input
background fixture's unused `oc_consumed_pads` variable under clang 23/Werror.
That fixture, its runner and host input source are unchanged from upstream;
no assertions or warning flags were weakened. The prior VRAM-budget and
HOS_FPS_LOG checks now pass with upstream's updates.

This merge uses upstream renderer API version 19. No complete API-19 game build,
new GPU play test or Linux build was performed. The earlier live menu approval
and API-18 game build are historical evidence, not validation of this revision.
The existing isolated game was left intact; an API-19 renderer should be used
with an API-19 host build. No proprietary file or generated image is included.

## Compact three-row comparison and merge through ba005ca4

Merged the user-approved comparison experiment and five upstream commits through
ba005ca4 without conflicts. Compare Advanced now separates the toolbar,
parameter carousel and value/slider into three rows. Up/Down changes rows;
Left/Right browses parameters or adjusts values. The carousel includes both
General and Quality, omits shortcut mode and Retry/Reset actions, and supports
mouse-wheel browsing and slider dragging. Save/Cancel retain transactional state.

The user tested the three-row interface in the running API-19 Windows game and
approved it. The complete game build succeeded before this final five-commit
merge (3:57 total, 1:31 compilation); the final menu renderer was then built and
loaded into that compatible host. No screenshots were captured by the agent.
No performance improvement, temporal-quality result or additional controller/
aspect-ratio coverage is claimed from that approval.

After the merge, Windows NGX renderer linking passes. The complete Windows suite
reports **463 passed, 3 failed, 34 skipped** in 114 seconds. All NR and affected
menu checks pass, including carousel enumeration, exclusions, navigation,
locking, bounds and Cancel restoration. The failures are Windows symlink
privileges, the clang-23 unused-global warning in the background-input fixture,
and the new upstream prompt-skip fixture attempting to write a manifest into a
missing prompts directory. The latter fixture and prompt-assets implementation
are identical to upstream ba005ca4; no tests were weakened. All six formerly
SDK-blocked .NET fixtures now pass with SDK 8.0.428 installed.

The final five upstream commits have compile/source-check coverage here; the
V-Sync/Frame Generation fix has no new live GPU validation from this run.
Linux validation and the earlier shutdown/TDR investigation remain limited as
previously documented. No proprietary artifacts are included.

## NR localization follow-up

Completed NR menu coverage in all 13 non-English translation tables (121 source
strings including labels, profile names, tooltips, Windows/Linux availability
errors, Compare status and input hints). Existing translations are reused.
The initial translations were machine-assisted; Italian text and ambiguous
technical labels were reviewed, with further native-speaker review welcome.
Only public menu text was sent for translation. There is no network translation
or additional dependency in the game.

Compare now translates labels and values before composing its counter/status
lines. Text fitting translates before clipping UTF-8 characters, preventing
truncated English lookup keys from silently reverting to English. Percentages,
DLL paths and environment switches remain unchanged. Saved keys are unaffected.

Windows NGX renderer linking passes. Complete suite: **464 passed, 3 failed,
34 skipped** in 111 seconds, with the same documented upstream/environment
failures. Final translation data refinements were followed by passing coverage
and compiled runtime-lookup tests in every locale and a successful renderer
rebuild. No GPU, Linux or all-locale visual layout verification is claimed.

## Merge through 9b6072cd

Integrated 60 upstream commits through
9b6072cd4b373bdd9ff3a8e056b7dc2b7dc359a5. Resolved conflicts in the menu,
translation tables, DLSS release path and audio/stick source-extraction fixtures.
Kept upstream's dynamic output-resolution/aspect choices, their persistence and
screen-follow behavior, and retirement of the Mesh shaders menu row. NR debug
remains session-only; Compare keeps its state masking and release-input guard.
Translation entries were merged by English key, preserving upstream removals and
updates as well as all NR translations; tables remain sorted with exact counts.

The combined DLSS path preserves the upstream refusal reset on release/retry.
Evaluation recovery now uses a release helper while already owning State::mu;
external release and retry acquire the mutex themselves. This prevents recursive
acquisition of the non-recursive mutex after repeated SR evaluation failures.
A CPU regression executes both release paths, deferred feature destruction and
retry. The NR menu fixture also checks dynamic custom-output indices and saved
player aspect/size values under process overrides. The README now lists the NR
submenu as required by upstream's new menu-table coverage test.

Windows Vulkan/NGX renderer linking passes. Final full suite:
**498 passed, 1 failed, 36 skipped** in 120 seconds. All NR, translation, menu,
output/aspect, recorder, DLSS and affected upstream checks pass. The remaining
failure is nx2/test_nup_paths.py: this Windows process lacks symbolic-link
creation privileges (WinError 1314). That fixture is unchanged from upstream.
The former input-background and prompt-skip failures now pass with main's fixes.
No assertions or compiler diagnostics were weakened.

Upstream's renderer interface is now API 21. No complete API-21 game build or
new GPU/Linux play test was performed for this merge. The existing isolated
API-19 game and renderer were left intact; the newly linked renderer must be
used with a matching host. Earlier API-19 live menu approval remains historical
evidence. NR comparison layout and interaction were not redesigned by this merge.
No screenshot or proprietary artifact was uploaded.


## Sectioned NR menu and shortcut removal (2026-11-12)

The experimental branch now uses a single page with the upstream menu's heading
rows: Base Options, Advanced Options, Masks, Compare and Diagnostics. Base orders
Visual profile before Style. All rendering parameters remain available, and the
Compare carousel follows the same order. The previous page/group selectors and
the F8/controller dedicated shortcut preference have been retired. Comparison
uses the toolbar NR control; Save/Cancel still commit/restore the transaction.
Old selector/shortcut keys are ignored without changing NR tuning preferences.
The historical General/Quality and shortcut descriptions above describe earlier
revisions, not the current menu.

All 13 non-English locales include the new headings and revised Compare help;
retired text has been removed. Regression checks assert section/setting order,
one-page visibility, absence of F8 capture, obsolete-key compatibility, and the
existing Compare/navigation/persistence behavior. Twelve targeted checks passed;
the complete Windows suite had 499 passed, 36 skipped and the unchanged
`nx2/test_nup_paths.py` failure caused by WinError 1314 (symlink privilege).
The Windows NGX renderer compiled successfully and was copied to the matching
API-21 isolated game folder while the game was closed. No screenshots, automated
game input or new visual/controller/Linux validation were performed. This change
has not been published to the existing PR.


## Model names, current main and Windows CI follow-up (2026-11-12)

Merged main through `4dd972a85ed5291ba7747ead68fbc38cdd69ec8e` (six commits),
including Auto (this PC), device-fault diagnostics and background gyro handling.
The save conflict preserves ownership of upstream auto-preset keys and retires
obsolete NR page/group/shortcut keys; the frame-generation fixture keeps both
upstream Auto and NR stand-ins. Existing assertions remain intact.

For NR 310.9.16, preset 0 (runtime default) and explicit preset 2 are displayed
as Model 2 (default), while preset 1 is Model 1. Preserved numeric values avoid
changing saved model selection. The October model is faster/smaller according
to NVIDIA; neither a universal quality deficit nor a Hookshot speedup is claimed.
The contract document links the NVIDIA announcement and the original runtime
verification report. New labels/help are translated in all 13 non-English
locales. The Compare carousel omits Neural Rendering, using its toolbar toggle.

The failed [Windows part 2 job](https://github.com/NotRexed/Hookshot/actions/runs/43286938930/job/131430377042?pr=1590)
failed compiling `tests/test_dlss_nr_menu.py`: MSVC CRT deprecates `tmpfile()` and
`-Werror` promotes that diagnostic to an error. The fixture now uses `tmpfile_s`
with the MSVC CRT and the portable stdio API elsewhere. No compiler warning or
assertion was disabled. The complete menu fixture passes locally with clang's
MSVC target and Visual Studio 2022 CRT/libraries, as well as llvm-mingw.

Fifteen targeted checks passed before the final full suite; the full Windows
suite completed with 501 passed, 36 skipped and the unchanged symlink-fixture
privilege failure in `nx2/test_nup_paths.py` (WinError 1314). The NGX renderer
compiled/linked successfully. The user reported successful live NR with DLSS
and FSR 3.1, Before and After, on the earlier experimental build; no same-scene
numbers or game area were supplied, so this is qualitative user feedback, not
an instrumented performance result. No Linux or additional GPU/controller
coverage is claimed for this merge. New hosted workflows must still validate
the published commit; local success is not a claim of green GitHub CI.

The complete matching Windows game build also succeeded after this merge
(61 seconds, compilation 19 seconds); it remains in the isolated local test
folder. This build result does not add a new visual or controller test.
