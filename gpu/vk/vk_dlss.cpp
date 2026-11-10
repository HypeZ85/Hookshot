// (hookshot) NVIDIA DLSS Super Resolution, one of the temporal upscaler's engines (vk_dlss.h). The frame chain it draws
// with (the scene's camera and depth, the inputs, the actor motion, the jitter, the picture into the network's output) is
// vk_temporal.cpp's; here are NGX, DLSS's feature and its evaluation.
//
// RX_VK_DLSS=0 / 1 / 2: the game's own, DLSS, DLAA over the Insert menu's Upscaler row (0 also leaves NGX's extensions
// off). DLSS's runtime (Windows: nvngx_dlss.dll; Linux: libnvidia-ngx-dlss.so.*) is looked for in RX_VK_DLSS_DIR=<folder>
// when set, the game folder's ngx folder, and %LOCALAPPDATA%\Hookshot\ngx (Linux: $XDG_DATA_HOME/hookshot/ngx): NGX takes
// the newest one it finds in them. Windows' paths are wide all the way (GetModuleFileNameW, _wgetenv), so any folder name
// works. On Linux NGX takes only ASCII paths: a folder named otherwise goes to it as /proc/self/fd/<n> (wide()), and the
// runtime itself has to be in a folder whose real path is ASCII (the log and the menu say so when it is not). The Insert
// menu's DLSS model row (opts.dlss_model, upscaler_rows.h) chooses DLSS's preset: K by default, M, L, or NVIDIA's default
// (0). RX_VK_DLSS_PRESET=<letter> (or 0) over it. RX_VK_DLSS_LOG=1: NGX's own log in ours.
//
// NVIDIA's NGX SDK is not part of Hookshot (NVIDIA's license): a local build fetches it (scripts/ngx_sdk.py) and builds with
// HOOKSHOT_NGX_SDK, which compiles this file with HOOKSHOT_NGX. Without it only the stubs at the end are here.
// NGX's core is the driver's (Windows: _nvngx.dll, which NVIDIA's loader finds in the driver's own folder; Linux:
// libnvidia-ngx.so). It is never put beside the game: a copy there would be loaded in place of the driver's.
#include "vk_dlss.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#ifdef _WIN32
#include <winver.h>
#else
#include <fcntl.h>   // (wide: a folder's descriptor for NGX)
#endif

#include "dlss_checks.h"
#include "vk_dump.h"
#include "shaders/dlss_nr_composite_comp_spv.h"
#include "upscaler_rows.h"
#include "vk_menu.h"
#include "vk_paths.h"
#include "vk_temporal.h"
#include "vk_texture.h"

#ifdef HOOKSHOT_NGX
#include "nvsdk_ngx_vk.h"
#include "nvsdk_ngx_helpers.h"
#include "nvsdk_ngx_helpers_vk.h"
#endif
#include "vk_work.h"   // (after the others: counts the commands that do GPU work, for full_barrier)

namespace mkwvk {

namespace {
int env_switch(const char* name) {   // (-1: unset)
    const char* e = getenv(name);
    return e && *e ? atoi(e) : -1;
}
}  // namespace

// (what is known of DLSS on this device: -1 not yet, else a DlssWhy. Set once the device is made and when NGX answers)
std::atomic<int> g_why{-1};
std::atomic<DlssNrStatus> g_nr_status{NR_WAITING};
std::atomic<dlss_nr::Issue> g_nr_issue{dlss_nr::Issue::none};
std::atomic<int> g_nr_compare{-1};
std::mutex nr_controls_mutex;
DlssNrControls nr_controls;
void dlss_nr_publish(const DlssNrControls& controls) {
    std::lock_guard lock(nr_controls_mutex);
    nr_controls = controls;
}
DlssNrStatus dlss_nr_status() { return g_nr_status.load(std::memory_order_relaxed); }
dlss_nr::Issue dlss_nr_issue() { return g_nr_issue.load(std::memory_order_relaxed); }
void dlss_nr_compare(int value) { g_nr_compare = std::clamp(value, -1, 1); }
void nr_issue_set(dlss_nr::Issue issue) {
    if (g_nr_issue.exchange(issue) != issue && issue != dlss_nr::Issue::none)
        logf("vk: DLSS 5 %s: %s", issue == dlss_nr::Issue::configuration_rejected ? "notice" : "unavailable", dlss_nr::message(issue));
}



DlssWhy dlss_why() {
    if (!dlss_built()) return DLSS_NOT_BUILT;
    static const bool env_off = env_switch("RX_VK_DLSS") == 0;
    if (env_off) return DLSS_ENV_OFF;
    const int w = g_why.load(std::memory_order_relaxed);
    return w < 0 ? DLSS_OK : (DlssWhy)w;
}

#ifdef HOOKSHOT_NGX

namespace {

/// NGX and DLSS's feature. The feature is used with the temporal upscaler's lock held; NGX's start (ngx_init) also from the
/// menu's thread (dlss_probe), under mu
struct Image : TemporalImage { VmaAllocation memory = nullptr; };

struct State {
    DlssNrControls nr_controls;
    TemporalImage color, depth, motion;
    VkSampler point = VK_NULL_HANDLE;
    // Experimental DLSS 5 Neural Rendering. Its output feeds SR/DLAA before the HUD.
    Image nr_output;
    Image nr_composed;
    VkDescriptorSetLayout nr_set = VK_NULL_HANDLE;
    VkPipelineLayout nr_layout = VK_NULL_HANDLE;
    VkPipeline nr_comp = VK_NULL_HANDLE;
    int nr_last_debug = 0;
    NVSDK_NGX_Parameter* nr_params = nullptr;
    NVSDK_NGX_Handle* nr_feature = nullptr;
    uint32_t nr_width = 0, nr_height = 0, nr_render_w = 0, nr_render_h = 0;
    float nr_jitter[2]{};
    bool nr_failed = false;
    uint64_t nr_frames = 0;
    dlss_nr::Edits nr_edits;
    uint64_t nr_created_frame = 0;
    bool nr_requested = false, nr_running = false;
    bool nr_checked = false;
    dlss_nr::Issue nr_support = dlss_nr::Issue::none;
    std::chrono::steady_clock::time_point nr_last_check{}, nr_off_since{};
    VkQueryPool nr_queries = VK_NULL_HANDLE;
    bool nr_timing_tried = false, nr_device_lost = false;
    struct NrQuery { bool pending = false; uint64_t serial = 0; } nr_query[16];
    uint32_t nr_query_cursor = 0;
    uint64_t nr_query_serial = 0, nr_sample_serial = 0;
    double nr_gpu_ms = -1;
    uint64_t nr_last_eval = 0;
    std::string nr_version;
    bool nr_rejected = false;
    dlss_nr::Settings nr_rejected_value;

    std::mutex mu;
    bool ngx_tried = false, ngx_ok = false;
    NVSDK_NGX_Parameter* caps = nullptr;
    NVSDK_NGX_Parameter* params = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    uint32_t feature_rw = 0, feature_rh = 0, feature_ow = 0, feature_oh = 0;   // (its largest render size, its output)
    uint32_t min_rw = 0, min_rh = 0;
    unsigned feature_preset = ~0u;   // (the preset it was made with: another one from the menu makes it again)
    uint64_t features_made = 0;      // (how many times: a feature made again in play costs a stall, so each says why)
};
State& st() {
    static State s;
    return s;
}

NVSDK_NGX_Resource_VK resource(const TemporalImage& im, bool write) {
    return NVSDK_NGX_Create_ImageView_Resource_VK(im.view, im.image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, im.format,
                                                  im.width, im.height, write);
}

void drop_image(Image& im) {
    if (!im.image) return;
    const Image old = im;
    temporal_later([old] {
        vkDestroyImageView(dev.device, old.view, nullptr);
        destroy_image(old.image, old.memory);
    });
    im = {};
}

bool make_image(VkCommandBuffer cmd, Image& im, VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags extra = 0) {
    if (im.image && im.format == format && im.width == w && im.height == h) return true;
    drop_image(im);
    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ic.imageType = VK_IMAGE_TYPE_2D, ic.format = format, ic.extent = {w, h, 1}, ic.mipLevels = 1, ic.arrayLayers = 1;
    ic.samples = VK_SAMPLE_COUNT_1_BIT, ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | extra;
    if (!create_image(ic, im.image, im.memory)) return im = {}, false;
    VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vc.image = im.image, vc.viewType = VK_IMAGE_VIEW_TYPE_2D, vc.format = format;
    vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(dev.device, &vc, nullptr, &im.view) != VK_SUCCESS) {
        destroy_image(im.image, im.memory);
        return im = {}, false;
    }
    im.format = format, im.width = w, im.height = h;
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};   // (UNDEFINED -> GENERAL, once)
    b.srcStageMask = VK_PIPELINE_STAGE_2_NONE, b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im.image, b.subresourceRange = vc.subresourceRange;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1, di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
    return true;
}

// Community-documented NGX feature 18. The public SDK names this slot Reserved18; the driver
// dispatches it to nvngx_dlssnr.dll. All calls go through NGX's normal API, with NVIDIA-signed
// runtimes; no snippet patching or third-party injection is used. See docs/dlss5-neural-rendering.md.
dlss_nr::Issue nr_check_locked(bool refresh);

void drop_nr() {
    State& s = st();
    if (s.nr_feature || s.nr_params) {
        NVSDK_NGX_Handle* feature = s.nr_feature;
        NVSDK_NGX_Parameter* params = s.nr_params;
        temporal_later([feature, params] {
            if (feature) NVSDK_NGX_VULKAN_ReleaseFeature(feature);
            if (params) NVSDK_NGX_VULKAN_DestroyParameters(params);
        });
        s.nr_feature = nullptr, s.nr_params = nullptr;
    }
    drop_image(s.nr_output);
    drop_image(s.nr_composed);
    if (s.nr_queries) {
        const auto pool = s.nr_queries;
        temporal_later([pool] { vkDestroyQueryPool(dev.device, pool, nullptr); });
        s.nr_queries = VK_NULL_HANDLE;
        for (auto& query : s.nr_query) query = {};
        s.nr_timing_tried = false;
        s.nr_gpu_ms = -1;
    }
    if (s.nr_comp || s.nr_layout || s.nr_set || s.point) {
        const auto pipe = s.nr_comp; const auto layout = s.nr_layout; const auto set = s.nr_set; const auto sampler = s.point;
        temporal_later([pipe, layout, set, sampler] {
            if (pipe) vkDestroyPipeline(dev.device, pipe, nullptr);
            if (layout) vkDestroyPipelineLayout(dev.device, layout, nullptr);
            if (set) vkDestroyDescriptorSetLayout(dev.device, set, nullptr);
            if (sampler) vkDestroySampler(dev.device, sampler, nullptr);
        });
        s.nr_comp = VK_NULL_HANDLE; s.nr_layout = VK_NULL_HANDLE; s.nr_set = VK_NULL_HANDLE; s.point = VK_NULL_HANDLE;
    }
    s.nr_width = s.nr_height = s.nr_render_w = s.nr_render_h = 0;
    s.nr_running = false;
    g_nr_status = s.nr_failed ? NR_FAILED : NR_WAITING;
}

bool neural_render(VkCommandBuffer cmd, uint32_t width, uint32_t height, bool& reset) {
    State& s = st();
    const int compare = g_nr_compare.load(std::memory_order_relaxed);
    const bool requested = compare >= 0 ? compare != 0 : s.nr_controls.requested;
    if (requested != s.nr_requested) {
        s.nr_requested = requested;
        logf("vk: DLSS 5 Neural Rendering: menu %s; evaluated frames %llu", s.nr_requested ? "On" : "Off", (unsigned long long)s.nr_frames);
        reset |= s.nr_running; // An unavailable request must not disturb SR history.
    }
    if (!s.nr_requested) {
        const auto off_now = std::chrono::steady_clock::now();
        if (menu_comparing() || s.nr_off_since == std::chrono::steady_clock::time_point{}) s.nr_off_since = off_now;
        if (s.nr_feature && !menu_comparing() && off_now - s.nr_off_since >= std::chrono::seconds(2)) {
            logf("vk: DLSS 5: releasing NR resources after two seconds Off outside Compare");
            drop_nr();
        }
        s.nr_running = false;
        g_nr_status = NR_WAITING;
        return false; // Comparison retains the model; ordinary Off releases it after a short grace period.
    }
    s.nr_off_since = {};
    if (s.nr_controls.native_hdr) {
        nr_issue_set(dlss_nr::Issue::native_hdr);
        reset |= s.nr_running;
        s.nr_running = false;
        g_nr_status = NR_WAITING;
        return false;
    }
    if (s.nr_failed) return false;
    const auto now = std::chrono::steady_clock::now();
    if (now - s.nr_last_check >= std::chrono::seconds(1)) {
        s.nr_last_check = now;
        if (const auto issue = nr_check_locked(false); issue != dlss_nr::Issue::none) {
            const bool changed = issue != dlss_nr_issue();
            reset |= s.nr_running;
            nr_issue_set(issue);
            if (s.nr_running || changed || s.nr_feature) drop_nr();
            return false;
        }
        nr_issue_set(s.nr_rejected ? dlss_nr::Issue::configuration_rejected : dlss_nr::Issue::none);
    }
    if (dlss_nr::blocking(dlss_nr_issue())) {
        reset |= s.nr_running;
        if (s.nr_feature) drop_nr();
        return false;
    }
    const int64_t ms_now = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    const dlss_nr::Settings wanted{s.nr_controls.model.intensity, s.nr_controls.model.structure, s.nr_controls.model.tone, s.nr_controls.model.skin,
                                  s.nr_controls.model.style, s.nr_controls.model.preset, s.nr_controls.model.mask, s.nr_controls.model.ui};
    const bool settled = s.nr_edits.observe(wanted, ms_now, menu_nr_editing());
    if (s.nr_rejected && wanted.clamped() == s.nr_edits.applied) {
        s.nr_rejected = false;
        nr_issue_set(dlss_nr::Issue::none);
    }
    // A drag never creates features. Even discrete edits are rate-limited and leave 32 evaluations between rebuilds.
    const bool rebuild = s.nr_feature && settled && s.nr_frames - s.nr_created_frame >= 32 &&
        (!s.nr_rejected || !(wanted.clamped() == s.nr_rejected_value));
    const bool pending = !(s.nr_edits.pending == s.nr_edits.applied);
    const auto refused = [&](const char* operation, NVSDK_NGX_Result result) {
        logf("vk: DLSS 5 Neural Rendering: %s failed (%#x); DLSS/DLAA continues without NR", operation, (unsigned)result);
        s.nr_failed = true;
        nr_issue_set(result == NVSDK_NGX_Result_FAIL_OutOfGPUMemory ? dlss_nr::Issue::out_of_memory :
                     result == NVSDK_NGX_Result_FAIL_FeatureNotSupported ? dlss_nr::Issue::unsupported_gpu : dlss_nr::Issue::render_failed);
        s.nr_checked = false;
        reset = true;
        drop_nr();
        return false;
    };
    const bool dimensions = s.feature_rw != s.nr_width || s.feature_rh != s.nr_height;
    if (!s.nr_feature || dimensions || rebuild) {
        // Build a candidate without releasing the last working feature. Failure cannot strand a live scene.
        NVSDK_NGX_Parameter* candidate_params = nullptr;
        NVSDK_NGX_Handle* candidate = nullptr;
        NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetCapabilityParameters(&candidate_params);
        const dlss_nr::Settings controls = s.nr_edits.pending;
        const auto t0 = std::chrono::steady_clock::now();
        if (NVSDK_NGX_SUCCEED(r) && candidate_params) {
            const auto ui = [&](const char* key, unsigned value) { NVSDK_NGX_Parameter_SetUI(candidate_params, key, value); };
            const auto fp = [&](const char* key, float value) { NVSDK_NGX_Parameter_SetF(candidate_params, key, value); };
            ui("DLSSNR.Enabled", 1), ui("DLSSNR.Width", s.feature_rw), ui("DLSSNR.Height", s.feature_rh);
            ui("CreationNodeMask", 1), ui("VisibilityNodeMask", 1);
            ui("DLSSNR.Hint.Render.Preset", controls.preset), ui("DLSSNR.Style", controls.style);
            ui("DLSSNR.UseAutoMask", controls.mask), ui("DLSSNR.UICorrection", controls.ui);
            fp("DLSSNR.Intensity", controls.intensity/100.0f);
            fp("DLSSNR.LocalStructureStrength", controls.structure/100.0f);
            fp("DLSSNR.LocalToneStrength", controls.tone/100.0f);
            fp("DLSSNR.SkinStructureStrength", controls.skin/100.0f);
            full_barrier(cmd);
            // Explicit test-only fault injection; absent in normal runs. Exercises rollback without exhausting VRAM.
#ifdef HOOKSHOT_NR_TESTING
            static const int reject_intensity = env_switch("RX_VK_NR_TEST_REJECT_INTENSITY");
#else
            constexpr int reject_intensity = -1;
#endif
            ::mkwvk::rec::boundary(::mkwvk::rec::B_DIRECT);
            r = reject_intensity >= 0 && controls.intensity == reject_intensity ? NVSDK_NGX_Result_FAIL_InvalidParameter :
                NVSDK_NGX_VULKAN_CreateFeature(cmd, (NVSDK_NGX_Feature)18, candidate_params, &candidate);
            note_work();
            full_barrier(cmd);
        }
        if (NVSDK_NGX_FAILED(r) || !candidate) {
            temporal_later([candidate, candidate_params] {
                if (candidate) NVSDK_NGX_VULKAN_ReleaseFeature(candidate);
                if (candidate_params) NVSDK_NGX_VULKAN_DestroyParameters(candidate_params);
            });
            if (!s.nr_feature || dimensions) return refused("candidate feature creation", r);
            s.nr_rejected = true, s.nr_rejected_value = controls;
            s.nr_edits.applied_ms = ms_now; // Rate-limit another distinct request too.
            nr_issue_set(dlss_nr::Issue::configuration_rejected);
            reset = true;
            logf("vk: NR candidate rejected (%#x), intensity %d style %d preset %d; retained last working feature",
                 (unsigned)r, controls.intensity, controls.style, controls.preset);
        } else {
            const auto old = s.nr_feature;
            const auto old_params = s.nr_params;
            if (old || old_params) temporal_later([old, old_params] {
                if (old) NVSDK_NGX_VULKAN_ReleaseFeature(old);
                if (old_params) NVSDK_NGX_VULKAN_DestroyParameters(old_params);
            });
            s.nr_feature = candidate, s.nr_params = candidate_params;
            s.nr_width = s.feature_rw, s.nr_height = s.feature_rh;
            s.nr_edits.commit(ms_now);
            s.nr_created_frame = s.nr_frames;
            s.nr_rejected = false;
            nr_issue_set(dlss_nr::Issue::none);
            reset = true;
            const double ms = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count();
            logf("vk: DLSS 5 Neural Rendering: feature created at %ux%u, intensity %.2f, style %d, preset %d, structure %.2f, tone %.2f, skin %.2f, mask %d, UI %d (%.1f ms CPU)",
                 width,height,controls.intensity/100.0f,controls.style,controls.preset,controls.structure/100.0f,
                 controls.tone/100.0f,controls.skin/100.0f,controls.mask,controls.ui,ms);
        }
    }
    const auto ui = [&](const char* key, unsigned value) { NVSDK_NGX_Parameter_SetUI(s.nr_params,key,value); };
    const auto fp = [&](const char* key, float value) { NVSDK_NGX_Parameter_SetF(s.nr_params,key,value); };
    if (!make_image(cmd, s.nr_output, VK_FORMAT_R16G16B16A16_SFLOAT, s.nr_width, s.nr_height, VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        return refused("output image creation", NVSDK_NGX_Result_Fail);
    NVSDK_NGX_Resource_VK color = resource(s.color, false), depth = resource(s.depth, false),
                          motion = resource(s.motion, false), output = resource(s.nr_output, true);
    NVSDK_NGX_Parameter_SetVoidPointer(s.nr_params, "DLSSNR.Color", &color);
    NVSDK_NGX_Parameter_SetVoidPointer(s.nr_params, "DLSSNR.Depth", &depth);
    NVSDK_NGX_Parameter_SetVoidPointer(s.nr_params, "DLSSNR.MVec", &motion);
    NVSDK_NGX_Parameter_SetVoidPointer(s.nr_params, "DLSSNR.Output", &output);
    // No verified DLSSNR jitter-offset key exists in the 310.9.16 contract.
    // Never accumulate history in jittered coordinates without that metadata; SR still gets its real jitter/history.
    const bool nr_reset = reset || !s.nr_running || s.nr_controls.history || s.nr_jitter[0] != 0 || s.nr_jitter[1] != 0 ||
        width != s.nr_render_w || height != s.nr_render_h;
    ui("DLSSNR.DepthInverted", 0), ui("DLSSNR.Reset", nr_reset ? 1 : 0);
    for (const char* prefix : {"Color", "Output", "Depth", "MVec"}) {
        char key[64];
        for (const char* axis : {"BaseX", "BaseY", "Width", "Height"}) {
            snprintf(key, sizeof key, "DLSSNR.%sSubrect%s", prefix, axis);
            ui(key, !strcmp(axis, "Width") ? width : !strcmp(axis, "Height") ? height : 0);
        }
    }
    fp("DLSSNR.MVecScaleX", 1.0f), fp("DLSSNR.MVecScaleY", 1.0f);
    full_barrier(cmd);
    // Host reset clears availability before recording reuse. Never WAIT for diagnostic timestamps.
    int timing = -1;
    if (s.nr_controls.stats && dev.host_query_reset && dev.timestamp_bits && vkResetQueryPool) {
        if (!s.nr_queries && !s.nr_timing_tried) {
            s.nr_timing_tried = true;
            VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            info.queryType = VK_QUERY_TYPE_TIMESTAMP, info.queryCount = 32;
            if (vkCreateQueryPool(dev.device, &info, nullptr, &s.nr_queries) != VK_SUCCESS) s.nr_queries = VK_NULL_HANDLE;
        }
        if (s.nr_queries) {
            for (unsigned i = 0; i < 16; ++i) if (s.nr_query[i].pending) {
                uint64_t values[4]{};
                const VkResult result = vkGetQueryPoolResults(dev.device, s.nr_queries, i * 2, 2, sizeof values, values, 16,
                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
                if (result == VK_ERROR_DEVICE_LOST) {
                    s.nr_device_lost = s.nr_failed = true;
                    nr_issue_set(dlss_nr::Issue::render_failed);
                    logf("vk: NR timestamps report VK_ERROR_DEVICE_LOST; leaving device-loss handling to the core renderer");
                    return false;
                }
                if (result == VK_SUCCESS && values[1] && values[3]) {
                    const uint64_t mask = dev.timestamp_bits == 64 ? ~0ull : (1ull << dev.timestamp_bits) - 1;
                    if (s.nr_query[i].serial > s.nr_sample_serial) {
                        s.nr_gpu_ms = ((values[2] - values[0]) & mask) * dev.props.limits.timestampPeriod / 1e6;
                        s.nr_sample_serial = s.nr_query[i].serial;
                    }
                    s.nr_query[i].pending = false;
                }
            }
            const unsigned slot = s.nr_query_cursor++ % 16;
            if (!s.nr_query[slot].pending) {
                timing = (int)slot;
                vkResetQueryPool(dev.device, s.nr_queries, slot * 2, 2);
                vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s.nr_queries, slot * 2);
            }
        }
    }
    ::mkwvk::rec::boundary(::mkwvk::rec::B_DIRECT);
    const NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_EvaluateFeature(cmd, s.nr_feature, s.nr_params, nullptr);
    note_work();
    full_barrier(cmd);
    if (timing >= 0) {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s.nr_queries, timing * 2 + 1);
        s.nr_query[timing].pending = true;
        s.nr_query[timing].serial = ++s.nr_query_serial;
    }
    if (NVSDK_NGX_FAILED(r)) return refused("evaluation", r);
    reset |= !s.nr_running; // Restart SR once when processed color replaces the original again.
    s.nr_running = true;
    s.nr_last_eval = texture_frame_no();
    s.nr_render_w = width, s.nr_render_h = height;
    g_nr_status = pending && !rebuild && !s.nr_rejected ? NR_PENDING : NR_ACTIVE;
    if (dump_on()) {   // (opt-in same-frame input/output evidence, through the existing dump machinery)
        Texture input, result;
        input.image = s.color.image, input.format = s.color.format, input.width = width, input.height = height;
        result.image = s.nr_output.image, result.format = s.nr_output.format, result.width = width, result.height = height;
        dump_image(cmd, &input, 0, 0, "dlss5_input");
        dump_image(cmd, &result, 0, 0, "dlss5_output");
    }
    if (++s.nr_frames == 1 || s.nr_frames % 600 == 0)
        logf("vk: DLSS 5 Neural Rendering evaluated: %llu frames at %ux%u before SR/DLAA (HUD excluded)",
             (unsigned long long)s.nr_frames, width, height);
    return true;
}

void NVSDK_CONV ngx_log(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    std::string m(message ? message : "");
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
    logf("ngx: %s", m.c_str());
}

namespace fs = std::filesystem;
#ifdef _WIN32
/// The game folder (game_module_path's rule: bin\hookshot.exe beside image.bin is the folder above bin), from
/// GetModuleFileNameW: a folder with characters outside the ANSI code page is still the right one
fs::path game_dir() {
    std::wstring exe(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, exe.data(), (DWORD)exe.size());
    if (!n || n >= exe.size()) return {};
    exe.resize(n);
    fs::path dir = fs::path(exe).parent_path();
    if (!_wcsicmp(dir.filename().c_str(), L"bin") && GetFileAttributesW((dir.parent_path() / L"image.bin").c_str()) != INVALID_FILE_ATTRIBUTES)
        dir = dir.parent_path();
    return dir;
}
/// An environment variable as a path (_wgetenv: not through the ANSI code page); empty when unset
fs::path env_path(const wchar_t* name) {
    const wchar_t* e = _wgetenv(name);
    return e && *e ? fs::path(e) : fs::path();
}
/// A path for our log (UTF-8)
std::string shown(const fs::path& p) {
    const std::wstring& w = p.native();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
/// A path for NGX, which takes wide strings: Windows' own
std::wstring wide(const fs::path& p) { return p.native(); }
#else
fs::path game_dir() {
    char exe[MAX_PATH];
    const DWORD n = game_module_path(exe, MAX_PATH);
    return fs::path(std::string(exe, n)).parent_path();
}
fs::path env_path(const char* name) {
    const char* e = getenv(name);
    return e && *e ? fs::path(e) : fs::path();
}
std::string shown(const fs::path& p) { return p.native(); }
/// A path for NGX, which takes wide strings. NGX's Linux library turns them back into bytes in the "C" locale
/// (WStrToUTF8Str: newlocale(LC_ALL, "C") and wcsrtombs), which fails on any character past ASCII, and the path comes out
/// empty. Before, each byte was also widened alone ("CafÃ©"). So a folder whose path is not all ASCII goes to NGX as
/// /proc/self/fd/<n>, the folder opened and kept open for the process's life: an ASCII name for the same folder, nothing
/// made on the disk. NGX writes its own files (the data path) and finds the runtime through it; the driver's part then
/// loads the runtime by its real path (realpath), which has to be ASCII too: ngx_init says so when that is all there is.
/// When the folder cannot be opened (not there), its path decoded from UTF-8 (dlss_checks.h, not through the locale; bytes
/// that are not UTF-8 become U+FFFD), for an NGX that takes it. Each such folder is said in the log
std::wstring wide(const fs::path& p) {
    const std::string& s = p.native();
    if (dlss_checks::ascii(s)) return std::wstring(s.begin(), s.end());
    if (const int fd = open(s.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); fd >= 0) {
        const std::string alias = "/proc/self/fd/" + std::to_string(fd);
        logf("vk: NVIDIA DLSS: %s is not all ASCII, which NGX's Linux library does not take: given to it as %s", s.c_str(),
             alias.c_str());
        return std::wstring(alias.begin(), alias.end());
    }
    bool bad = false;
    std::wstring w = dlss_checks::utf8_to_wide(s, &bad);
    logf("vk: NVIDIA DLSS: %s is not all ASCII and could not be opened: given to NGX as it is%s", s.c_str(),
         bad ? " (not UTF-8: U+FFFD for its bad bytes)" : "");
    return w;
}
#endif

#ifdef _WIN32
constexpr const char* kRuntime = "nvngx_dlss.dll";   // (DLSS's runtime, as scripts/ngx_sdk.py fetches it)
constexpr const fs::path::value_type* kRuntimeName = L"nvngx_dlss.dll";
#else
constexpr const char* kRuntime = "libnvidia-ngx-dlss.so";
constexpr const fs::path::value_type* kRuntimeName = "libnvidia-ngx-dlss.so";
#endif

/// NGX on this device, once: false when DLSS cannot run here (the reason logged)
bool ngx_init() {
    State& s = st();
    if (s.ngx_tried) return s.ngx_ok;
    s.ngx_tried = true;
    // (the game folder or nothing: an empty one made "ngx" relative, and NGX looked for its runtime in the current folder)
    const fs::path game = game_dir();
    if (game.empty() || game.is_relative()) {
        logf("vk: NVIDIA DLSS: the game's folder could not be found: no DLSS");
        g_why = DLSS_NO_RUNTIME;
        return false;
    }
    // where DLSS's runtime (kRuntime) is looked for (NGX takes the newest it finds), and its own files go
    std::vector<fs::path> dirs;
#ifdef _WIN32
    if (fs::path e = env_path(L"RX_VK_DLSS_DIR"); !e.empty()) dirs.push_back(e);
    dirs.push_back(game / L"ngx");
    if (fs::path l = env_path(L"LOCALAPPDATA"); !l.empty()) dirs.push_back(l / L"Hookshot" / L"ngx");
#else
    if (fs::path e = env_path("RX_VK_DLSS_DIR"); !e.empty()) dirs.push_back(e);
    dirs.push_back(game / "ngx");
    if (fs::path x = env_path("XDG_DATA_HOME"); !x.empty()) dirs.push_back(x / "hookshot" / "ngx");
    else if (fs::path h = env_path("HOME"); !h.empty()) dirs.push_back(h / ".local" / "share" / "hookshot" / "ngx");
#endif
    std::vector<std::wstring> wdirs;
    for (const auto& d : dirs) wdirs.push_back(wide(d));
    std::vector<const wchar_t*> paths;
    for (const auto& d : wdirs) paths.push_back(d.c_str());
    const fs::path data = game / "ngx";
    std::error_code made;   // (not the throwing overload: a folder that cannot be made, read-only or so, must not end the game)
    fs::create_directories(data, made);   // (NGX's logs and its own files: the game folder's, not /usr/share)
    const std::wstring wdata = wide(data);
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = paths.data();
    info.PathListInfo.Length = (unsigned)paths.size();
    static const bool verbose = env_switch("RX_VK_DLSS_LOG") > 0;
    info.LoggingInfo.LoggingCallback = verbose ? ngx_log : nullptr;
    info.LoggingInfo.MinimumLoggingLevel = verbose ? NVSDK_NGX_LOGGING_LEVEL_ON : NVSDK_NGX_LOGGING_LEVEL_OFF;
    // (NGX also writes its own log files into the data folder; with RX_VK_DLSS_LOG=1 its lines go to ours instead)
    info.LoggingInfo.DisableOtherLoggingSinks = verbose;
    // (a project id of Hookshot's own: NGX wants one with the custom engine type, any GUID)
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_Init_with_ProjectID("7b2f5d0c-6a1e-4c39-9f84-3d5e2a1b0c96", NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                              "1.0", wdata.c_str(), dev.instance, dev.physical, dev.device,
                                                              instance_proc(), vkGetDeviceProcAddr, &info);
    if (NVSDK_NGX_FAILED(r)) {
        logf("vk: NVIDIA DLSS: NGX did not start (%#x): not an NVIDIA GPU, or its driver has no NGX", (unsigned)r);
        g_why = DLSS_NEEDS_RTX;
        return false;
    }
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetCapabilityParameters(&s.caps)) || !s.caps) {
        logf("vk: NVIDIA DLSS: NGX has no capabilities for this device");
        g_why = DLSS_NEEDS_RTX;
        return false;
    }
    // (NGX's C functions, not NVSDK_NGX_Parameter's virtual ones: its object comes from NVIDIA's MSVC-built library, whose
    // vtable order is not the Itanium ABI's that llvm-mingw calls with. The overloads only lined up by luck)
    int available = 0, needs_driver = 0;
    unsigned major = 0, minor = 0;
    NVSDK_NGX_Parameter_GetI(s.caps, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    NVSDK_NGX_Parameter_GetI(s.caps, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
    NVSDK_NGX_Parameter_GetUI(s.caps, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &major);
    NVSDK_NGX_Parameter_GetUI(s.caps, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minor);
    std::string where;
    for (const auto& d : dirs) where += (where.empty() ? "" : ", ") + shown(d);
    if (!available) {
        // (for the menu: a runtime found in none of the folders is the likely reason; with one there, the GPU (a GTX))
        int init = 0;
        NVSDK_NGX_Parameter_GetI(s.caps, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &init);
        // (Linux: NGX loads the runtime by its real path, ASCII only (wide()): one only in folders named otherwise is not
        // loaded, FeatureInitResult 0xbad00004, though the GPU runs DLSS)
        bool runtime = false, loadable = false;
        std::error_code ec;
        for (const auto& d : dirs)
            for (const auto& f : fs::directory_iterator(d, ec))
                if (f.path().filename().native().rfind(kRuntimeName, 0) == 0) {
                    runtime = true;
#ifdef _WIN32
                    loadable = true;
#else
                    std::error_code real;
                    loadable |= dlss_checks::ascii(fs::canonical(f.path(), real).native()) && !real;
#endif
                }
        if (needs_driver) logf("vk: NVIDIA DLSS: needs NVIDIA's driver %u.%u or newer", major, minor);
        else if (!runtime) logf("vk: NVIDIA DLSS: not available: no %s in %s (scripts/ngx_sdk.py fetches it)", kRuntime, where.c_str());
        else if (!loadable)
            logf("vk: NVIDIA DLSS: not available: %s is only in folders whose path is not all ASCII (%s), and NVIDIA's NGX on "
                 "Linux loads it only from one that is: put it in $XDG_DATA_HOME/hookshot/ngx, or RX_VK_DLSS_DIR=<an ASCII folder>",
                 kRuntime, where.c_str());
        else logf("vk: NVIDIA DLSS: not available on this GPU (%#x): DLSS needs an NVIDIA RTX GPU", (unsigned)init);
        g_why = needs_driver ? DLSS_NEEDS_DRIVER : !runtime ? DLSS_NO_RUNTIME : !loadable ? DLSS_RUNTIME_NOT_ASCII : DLSS_NEEDS_RTX;
        return false;
    }
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_AllocateParameters(&s.params)) || !s.params) return false;
    logf("vk: NVIDIA DLSS: NGX started (DLSS's runtime: the newest %s in %s)", kRuntime, where.c_str());
    s.ngx_ok = true;
    g_why = DLSS_OK;
    return true;
}

// Called with State::mu held. A local NR file is mandatory even if NGX can search other directories.
dlss_nr::Issue nr_check_locked(bool refresh) {
    using dlss_nr::Issue;
    State& s = st();
    if (s.nr_device_lost) return Issue::upstream_failed;
    dlss_nr::Requirements requirements;
#ifdef _WIN32
    const fs::path runtime = game_dir() / "ngx" / "nvngx_dlssnr.dll";
    std::error_code ec;
    const bool exists = fs::exists(runtime, ec);
    const bool valid_file = exists && fs::is_regular_file(runtime, ec) && fs::file_size(runtime, ec) > 0 && !ec;
    requirements.runtime = exists;
    requirements.valid_file = valid_file;
#elif defined(__linux__)
    static const bool experimental = env_switch("RX_VK_DLSS_NR_LINUX") > 0;
    if (const auto issue = dlss_nr::linux_gate(experimental); issue != Issue::none) return issue;
    // Native NGX owns Linux feature discovery/loading through its existing search paths.
    // Do not guess a private .so filename, dlopen a PE DLL, or use a compatibility layer.
    // A successful DLSSNR.Available query below is mandatory before CreateFeature(18).
#else
    return Issue::platform;
#endif
    requirements.nvidia = dev.props.vendorID == 0x10de;
    requirements.upscaler = temporal_mode() == 1 || temporal_mode() == 2;
    switch (dlss_why()) {
    case DLSS_NOT_BUILT: requirements.upstream = Issue::not_built; break;
    case DLSS_ENV_OFF: requirements.upstream = Issue::disabled; break;
    case DLSS_NEEDS_RTX: requirements.upstream = Issue::unsupported_gpu; break;
    case DLSS_NEEDS_DRIVER: requirements.upstream = Issue::needs_driver; break;
    case DLSS_NO_RUNTIME: requirements.upstream = Issue::missing_dlss_runtime; break;
    case DLSS_RUNTIME_NOT_ASCII: requirements.upstream = Issue::runtime_path; break;
    default: break;
    }
    if (const Issue issue = dlss_nr::prerequisite(requirements); issue != Issue::none) return issue;
    if (s.nr_controls.native_hdr) return Issue::native_hdr;
    if (refresh || s.nr_version.empty()) {
        s.nr_version = "unknown";
#ifdef _WIN32
        const HMODULE version = LoadLibraryW(L"version.dll");
        using Size = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
        using Info = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
        using Query = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
        const auto size = version ? (Size)GetProcAddress(version, "GetFileVersionInfoSizeW") : nullptr;
        const auto info = version ? (Info)GetProcAddress(version, "GetFileVersionInfoW") : nullptr;
        const auto query = version ? (Query)GetProcAddress(version, "VerQueryValueW") : nullptr;
        DWORD unused = 0, bytes = size ? size(runtime.c_str(), &unused) : 0;
        if (info && query && bytes && bytes < 16*1024*1024) {
            std::vector<uint8_t> data(bytes);
            VS_FIXEDFILEINFO* value = nullptr; UINT length = 0;
            if (info(runtime.c_str(), 0, bytes, data.data()) && query(data.data(), L"\\", (void**)&value, &length) &&
                length >= sizeof(VS_FIXEDFILEINFO)) {
                char text[48];
                snprintf(text, sizeof text, "%u.%u.%u.%u", HIWORD(value->dwFileVersionMS), LOWORD(value->dwFileVersionMS),
                         HIWORD(value->dwFileVersionLS), LOWORD(value->dwFileVersionLS));
                s.nr_version = text;
            }
        }
        if (version) FreeLibrary(version);
#elif defined(__linux__)
        s.nr_version = "native NGX (unreported)";
#endif
    }
    if (!dev.device) return Issue::unavailable;
    if (!ngx_init()) {
        switch (dlss_why()) {
        case DLSS_NEEDS_RTX: return Issue::unsupported_gpu;
        case DLSS_NEEDS_DRIVER: return Issue::needs_driver;
        case DLSS_NO_RUNTIME: return Issue::missing_dlss_runtime;
        default: return Issue::unavailable;
        }
    }
    if (s.nr_checked && !refresh) return s.nr_support;
    NVSDK_NGX_Parameter* caps = nullptr;
    const NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetCapabilityParameters(&caps);
    int available = 0, init = 0, needs_driver = 0;
    NVSDK_NGX_Result queried = NVSDK_NGX_Result_Fail;
    if (NVSDK_NGX_SUCCEED(r) && caps) {
        queried = NVSDK_NGX_Parameter_GetI(caps, "DLSSNR.Available", &available);
        NVSDK_NGX_Parameter_GetI(caps, "DLSSNR.FeatureInitResult", &init);
        // Optional key: older NR runtimes need not supply it; missing does not mean a driver failure.
        NVSDK_NGX_Parameter_GetI(caps, "DLSSNR.NeedsUpdatedDriver", &needs_driver);
    }
#if defined(__linux__) && !defined(_WIN32)
    s.nr_support = dlss_nr::linux_capability(NVSDK_NGX_SUCCEED(queried), available != 0,
                                      (unsigned)init == NVSDK_NGX_Result_FAIL_FeatureNotSupported, needs_driver != 0);
#else
    s.nr_support = dlss_nr::capability(NVSDK_NGX_SUCCEED(queried), available != 0,
                                      (unsigned)init == NVSDK_NGX_Result_FAIL_FeatureNotSupported, needs_driver != 0);
#endif
    s.nr_checked = true;
    logf("vk: DLSS 5 preflight: available %d, init %#x, query %#x, driver update %d: %s", available, (unsigned)init,
         (unsigned)queried, needs_driver, s.nr_support == Issue::none ? "ready" : dlss_nr::message(s.nr_support));
    if (caps) NVSDK_NGX_VULKAN_DestroyParameters(caps);
    return s.nr_support;
}

/// One compute pipeline with push descriptors; what it made is destroyed again when a step fails
bool make_pipeline(const uint32_t* code, size_t size, const VkDescriptorType* types, uint32_t n, uint32_t push,
                   VkDescriptorSetLayout& set, VkPipelineLayout& layout, VkPipeline& pipe) {
    const auto undo = [&] {
        if (layout) vkDestroyPipelineLayout(dev.device, layout, nullptr);
        if (set) vkDestroyDescriptorSetLayout(dev.device, set, nullptr);
        set = VK_NULL_HANDLE, layout = VK_NULL_HANDLE, pipe = VK_NULL_HANDLE;
        return false;
    };
    VkDescriptorSetLayoutBinding b[8]{};
    for (uint32_t i = 0; i < n; i++) b[i] = {i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = n, li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &set) != VK_SUCCESS) return set = VK_NULL_HANDLE, undo();
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, push};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1, pi.pSetLayouts = &set, pi.pushConstantRangeCount = 1, pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev.device, &pi, nullptr, &layout) != VK_SUCCESS) return layout = VK_NULL_HANDLE, undo();
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = size, mi.pCode = code;
    VkShaderModule m{};
    if (vkCreateShaderModule(dev.device, &mi, nullptr, &m) != VK_SUCCESS) return undo();
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", nullptr};
    ci.layout = layout;
    const bool ok = vkCreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipe) == VK_SUCCESS;
    vkDestroyShaderModule(dev.device, m, nullptr);
    return ok || undo();
}

bool nr_composite(VkCommandBuffer cmd, uint32_t width, uint32_t height, bool neural, const TemporalImage& output) {
    State& s = st();
    const VkDescriptorType types[] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    if (!s.nr_comp && !make_pipeline(DLSS_NR_COMPOSITE_COMP_SPV, sizeof DLSS_NR_COMPOSITE_COMP_SPV, types, 5, 24,
                                    s.nr_set, s.nr_layout, s.nr_comp)) return false;
    if (!s.point) {
        VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        info.magFilter = info.minFilter = VK_FILTER_NEAREST;
        info.addressModeU = info.addressModeV = info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(dev.device, &info, nullptr, &s.point) != VK_SUCCESS) return false;
    }
    if (!s.nr_controls.debug && !make_image(cmd, s.nr_composed, VK_FORMAT_R16G16B16A16_SFLOAT, s.feature_rw, s.feature_rh)) return false;
    const TemporalImage& target = s.nr_controls.debug ? output : s.nr_composed;
    const uint32_t dw = s.nr_controls.debug ? output.width : width, dh = s.nr_controls.debug ? output.height : height;
    struct Push { int32_t w, h, iw, ih; float color; int32_t debug; } pc{(int32_t)dw, (int32_t)dh, (int32_t)width, (int32_t)height,
        std::clamp(s.nr_controls.color, 0, 100) / 100.0f, s.nr_controls.debug};
    if (!neural && (pc.debug == 2 || pc.debug == 3)) pc.debug = 1;
    const VkDescriptorImageInfo info[] = {{s.point, s.color.view, VK_IMAGE_LAYOUT_GENERAL},
        {s.point, neural ? s.nr_output.view : s.color.view, VK_IMAGE_LAYOUT_GENERAL},
        {s.point, s.depth.view, VK_IMAGE_LAYOUT_GENERAL}, {s.point, s.motion.view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, target.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[5]{};
    for (unsigned i = 0; i < 5; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstBinding = i, writes[i].descriptorCount = 1, writes[i].descriptorType = types[i];
        writes[i].pImageInfo = &info[i];
    }
    full_barrier(cmd);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.nr_comp);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.nr_layout, 0, 5, writes);
    vkCmdPushConstants(cmd, s.nr_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cmd, (dw + 7) / 8, (dh + 7) / 8, 1);
    full_barrier(cmd);
    return true;
}

/// DLSS's preset: the DLSS model row's, RX_VK_DLSS_PRESET over it (upscaler_rows.h dlss_preset)
unsigned wanted_preset() {
    static const char* const env = getenv("RX_VK_DLSS_PRESET");
    return upscaler_rows::dlss_preset(opts.dlss_model, env && *env ? env : nullptr);
}

/// DLSS's feature for this output size and preset, made when either changes (cmd: its creation's work): the engine's
/// prepare (vk_temporal.h)
int prepare(VkCommandBuffer cmd, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, uint32_t* in_w, uint32_t* in_h,
            const char** fail) {
    State& s = st();
    std::lock_guard lock(s.mu);
    const unsigned preset = wanted_preset();
    if (s.feature && s.feature_ow == ow && s.feature_oh == oh && rw <= s.feature_rw && rh <= s.feature_rh &&
        rw >= s.min_rw && rh >= s.min_rh && s.feature_preset == preset)
        return *in_w = s.feature_rw, *in_h = s.feature_rh, 1;
    // (why it is made: in the log with its time, so a stall in play can be matched with one)
    char why[160];
    if (!s.feature && !s.features_made) snprintf(why, sizeof why, "the first");
    else if (!s.feature) snprintf(why, sizeof why, "again after it was let go");
    else if (s.feature_ow != ow || s.feature_oh != oh)
        snprintf(why, sizeof why, "the output went from %ux%u to %ux%u", s.feature_ow, s.feature_oh, ow, oh);
    else if (s.feature_preset != preset)
        snprintf(why, sizeof why, "preset %c, was %c (the DLSS model row)", upscaler_rows::preset_letter(preset),
                 upscaler_rows::preset_letter(s.feature_preset));
    else
        snprintf(why, sizeof why, "the render size %ux%u left its range, %ux%u to %ux%u", rw, rh, s.min_rw, s.min_rh,
                 s.feature_rw, s.feature_rh);
    if (s.feature) {
        NVSDK_NGX_Handle* old = s.feature;
        temporal_later([old] { NVSDK_NGX_VULKAN_ReleaseFeature(old); });
        s.feature = nullptr;
    }
    // (the quality mode whose range of render sizes holds this one: the network's 3/4 is DLSS's MaxQuality range)
    // (DLAA's when the render size is the output's)
    static const NVSDK_NGX_PerfQuality_Value scaled[] = {NVSDK_NGX_PerfQuality_Value_MaxQuality, NVSDK_NGX_PerfQuality_Value_DLAA,
                                                        NVSDK_NGX_PerfQuality_Value_Balanced, NVSDK_NGX_PerfQuality_Value_MaxPerf,
                                                        NVSDK_NGX_PerfQuality_Value_UltraPerformance};
    static const NVSDK_NGX_PerfQuality_Value same[] = {NVSDK_NGX_PerfQuality_Value_DLAA, NVSDK_NGX_PerfQuality_Value_MaxQuality};
    const bool aa = rw == ow && rh == oh;
    const NVSDK_NGX_PerfQuality_Value* modes = aa ? same : scaled;
    const size_t nmodes = aa ? 2 : 5;
    NVSDK_NGX_PerfQuality_Value mode = modes[0];
    unsigned max_w = 0, max_h = 0, min_w = 0, min_h = 0;
    bool found = aa;
    if (aa) max_w = min_w = ow, max_h = min_h = oh;   // (DLAA: the render size is the output's, one size)
    for (size_t i = 0; i < nmodes && !found; i++) {
        const NVSDK_NGX_PerfQuality_Value m = modes[i];
        unsigned w = 0, h = 0, xw = 0, xh = 0, nw = 0, nh = 0;
        float sharp = 0;
        if (NVSDK_NGX_FAILED(NGX_DLSS_GET_OPTIMAL_SETTINGS(s.caps, ow, oh, m, &w, &h, &xw, &xh, &nw, &nh, &sharp)) || !w) continue;
        if (rw >= nw && rh >= nh && rw <= xw && rh <= xh) {
            mode = m, max_w = xw, max_h = xh, min_w = nw, min_h = nh, found = true;
            break;
        }
    }
    if (!found) return *fail = "no DLSS mode takes this render size", 0;
    // (the DLSS model row's preset for every mode, K by default: NVIDIA's transformer model. 0: NVIDIA's own per mode)
    static_assert(NVSDK_NGX_DLSS_Hint_Render_Preset_K == 11 && NVSDK_NGX_DLSS_Hint_Render_Preset_L == 12 &&
                  NVSDK_NGX_DLSS_Hint_Render_Preset_M == 13 && NVSDK_NGX_DLSS_Hint_Render_Preset_Default == 0);
    for (const char* k : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                          NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                          NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
                          NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality})
        NVSDK_NGX_Parameter_SetUI(s.params, k, preset);
    NVSDK_NGX_DLSS_Create_Params cp{};
    cp.Feature.InWidth = max_w, cp.Feature.InHeight = max_h, cp.Feature.InTargetWidth = ow, cp.Feature.InTargetHeight = oh;
    cp.Feature.InPerfQualityValue = mode;
    // (LDR colour, already tonemapped: no exposure; motion at the render size, not jittered; depth not reversed)
    cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    const auto t0 = std::chrono::steady_clock::now();
    ::mkwvk::rec::boundary(::mkwvk::rec::B_DIRECT);   // (NGX records into cmd itself: what this thread recorded through vk_work.h reaches cmd first)
    const NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSS_EXT(cmd, 1, 1, &s.feature, s.params, &cp);
    const double made_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    note_work();   // (NGX's own commands, which vk_work.h does not see: the next full_barrier is not left out)
    if (NVSDK_NGX_FAILED(r) || !s.feature) {
        logf("vk: NVIDIA DLSS: its feature was not made (%#x) for %ux%u -> %ux%u", (unsigned)r, max_w, max_h, ow, oh);
        s.feature = nullptr;
        return *fail = "its feature was not made", -1;
    }
    s.feature_rw = max_w, s.feature_rh = max_h, s.feature_ow = ow, s.feature_oh = oh, s.min_rw = min_w, s.min_rh = min_h;
    s.feature_preset = preset;
    s.features_made++;
    logf("vk: NVIDIA DLSS: feature %llu made (%s; frame %llu, %.1f ms of CPU): %ux%u (%ux%u to %ux%u) -> %ux%u, mode %d, "
         "preset %c%s", (unsigned long long)s.features_made, why, (unsigned long long)texture_frame_no(), made_ms, rw, rh,
         min_w, min_h, max_w, max_h, ow, oh, (int)mode, upscaler_rows::preset_letter(preset),
         preset ? "" : " (NVIDIA's default)");
    *in_w = s.feature_rw, *in_h = s.feature_rh;
    return 2;   // (a new history)
}

bool start() { std::lock_guard lock(st().mu); return ngx_init(); }

bool evaluate(VkCommandBuffer cmd, const TemporalFrame& f) {
    State& s = st();
    std::lock_guard lock(s.mu);
    { std::lock_guard controls_lock(nr_controls_mutex); s.nr_controls = nr_controls; }
    s.color = f.color, s.depth = f.depth, s.motion = f.motion;
    s.nr_jitter[0] = f.jitter[0], s.nr_jitter[1] = f.jitter[1];
    bool reset = f.reset;
    if (s.nr_last_debug != s.nr_controls.debug) reset = true, s.nr_last_debug = s.nr_controls.debug;
    const bool buffer_view = s.nr_controls.debug == 1 || s.nr_controls.debug >= 4;
    const bool neural = !buffer_view && neural_render(cmd, f.render_w, f.render_h, reset);
    if (s.nr_device_lost) return false;
    if (buffer_view) { reset |= s.nr_running; s.nr_running = false; g_nr_status = NR_WAITING; }
    const bool needs_composite = s.nr_controls.debug || (neural && s.nr_controls.color != 100);
    const bool composed = needs_composite && nr_composite(cmd, f.render_w, f.render_h, neural, f.output);
    if (needs_composite && !composed) {
        nr_issue_set(dlss_nr::Issue::render_failed);
        s.nr_failed = true;
        reset |= s.nr_running;
        drop_nr();
    }
    // Debug buffers are written directly into the engine output, never processed by NR or SR.
    if (s.nr_controls.debug && composed) return true;
    NVSDK_NGX_Resource_VK color = resource(composed ? static_cast<const TemporalImage&>(s.nr_composed) :
        neural && !needs_composite ? static_cast<const TemporalImage&>(s.nr_output) : f.color, false),
        depth = resource(f.depth, false), mv = resource(f.motion, false), output = resource(f.output, true);
    NVSDK_NGX_VK_DLSS_Eval_Params ep{};
    ep.Feature.pInColor = &color, ep.Feature.pInOutput = &output;
    ep.pInDepth = &depth, ep.pInMotionVectors = &mv;
    ep.InJitterOffsetX = f.jitter[0], ep.InJitterOffsetY = f.jitter[1];
    ep.InRenderSubrectDimensions = {f.render_w, f.render_h};
    ep.InReset = reset ? 1 : 0;
    ep.InMVScaleX = ep.InMVScaleY = 1.0f;
    ::mkwvk::rec::boundary(::mkwvk::rec::B_DIRECT);
    const NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSS_EXT(cmd, s.feature, s.params, &ep);
    note_work();
    if (NVSDK_NGX_FAILED(r)) {
        logf("vk: NVIDIA DLSS: its evaluation failed (%#x)", (unsigned)r);
        return false;
    }
    return true;
}

void release() {
    State& s = st();
    std::lock_guard lock(s.mu);
    drop_nr();
    if (!s.feature) return;
    NVSDK_NGX_Handle* old = s.feature;
    temporal_later([old] { NVSDK_NGX_VULKAN_ReleaseFeature(old); });
    s.feature = nullptr;
    s.feature_rw = s.feature_rh = s.feature_ow = s.feature_oh = s.min_rw = s.min_rh = 0;
}

uint64_t memory() {
    State& s = st();
    unsigned long long ngx = 0;
    return s.caps && NVSDK_NGX_SUCCEED(NGX_DLSS_GET_STATS(s.caps, &ngx)) ? (uint64_t)ngx : 0;
}

const TemporalEngine kEngine{"NVIDIA DLSS", "DLSS", start, prepare, evaluate, release, memory};

}  // namespace

dlss_nr::Issue dlss_nr_check(bool refresh) {
    State& s = st();
    std::lock_guard lk(s.mu);
    if (s.nr_failed && !refresh) return dlss_nr_issue(); // Capability success does not erase an evaluation failure.
    const auto issue = nr_check_locked(refresh);
    if (issue == dlss_nr::Issue::none && refresh) s.nr_rejected = false;
    const auto shown = issue == dlss_nr::Issue::none && s.nr_rejected ? dlss_nr::Issue::configuration_rejected : issue;
    nr_issue_set(shown);
    if (issue == dlss_nr::Issue::none && refresh) s.nr_failed = false;
    return shown;
}
void dlss_nr_retry() { dlss_nr_check(true); }

DlssNrStats dlss_nr_stats() {
    State& s = st();
    std::lock_guard lk(s.mu);
    DlssNrStats result;
    result.gpu_ms = s.nr_running ? s.nr_gpu_ms : 0;
    result.width = s.nr_render_w, result.height = s.nr_render_h, result.frames = s.nr_frames;
    result.active = s.nr_running && s.nr_requested && (temporal_mode() == 1 || temporal_mode() == 2) && texture_frame_no()-s.nr_last_eval <= 2 && !dlss_nr::blocking(dlss_nr_issue());
    for (const Image* image : {&s.nr_output, &s.nr_composed}) if (image->image) result.images_bytes += allocation_size(image->memory);
    snprintf(result.runtime, sizeof result.runtime, "%s", s.nr_version.empty() ? "unknown" : s.nr_version.c_str());
    return result;
}

bool dlss_built() { return true; }
const TemporalEngine* dlss_engine() { return &kEngine; }

void dlss_instance_extensions(std::vector<const char*>& want, const std::vector<VkExtensionProperties>& all) {
    if (env_switch("RX_VK_DLSS") == 0) return;
    for (const auto& e : all)
        if (!strcmp(e.extensionName, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
            want.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
            return;
        }
}

void dlss_device_extensions(std::vector<const char*>& want, const std::vector<VkExtensionProperties>& all) {
    // (NGX's: VK_NVX_binary_import and VK_NVX_image_view_handle, with push descriptors and buffer device addresses,
    // which the replay enables already: core 1.2's bufferDeviceAddress stands for VK_EXT_buffer_device_address. The NVX ones
    // are on whenever the device has them, so the Insert menu's row needs no restart)
    if (env_switch("RX_VK_DLSS") == 0) return;
    bool both = true;
    for (const char* name : {VK_NVX_BINARY_IMPORT_EXTENSION_NAME, VK_NVX_IMAGE_VIEW_HANDLE_EXTENSION_NAME}) {
        bool have = false, listed = false;
        for (const auto& e : all) have |= !strcmp(e.extensionName, name);
        for (const char* w : want) listed |= !strcmp(w, name);
        if (have && !listed) want.push_back(name);
        both &= have;
    }
    // (NVIDIA's own extensions: without them this is not an NVIDIA GPU, or not one NGX runs on. With them, NGX says, in
    // dlss_probe or at the first upscale: a GTX has them too)
    if (!both) g_why = DLSS_NEEDS_RTX;
}

void dlss_probe() {
    if (dlss_why() != DLSS_OK || g_why.load(std::memory_order_relaxed) >= 0 || !dev.device) return;
    std::lock_guard lock(st().mu);
    ngx_init();
}

#else  // (no NGX SDK in this build: DLSS is not there)

bool dlss_built() { return false; }
dlss_nr::Issue dlss_nr_check(bool) { nr_issue_set(dlss_nr::Issue::not_built); return dlss_nr::Issue::not_built; }
void dlss_nr_retry() { nr_issue_set(dlss_nr::Issue::not_built); }
DlssNrStats dlss_nr_stats() { return {}; }

const TemporalEngine* dlss_engine() { return nullptr; }
void dlss_probe() {}
void dlss_instance_extensions(std::vector<const char*>&, const std::vector<VkExtensionProperties>&) {}
void dlss_device_extensions(std::vector<const char*>&, const std::vector<VkExtensionProperties>&) {}

#endif

} // namespace mkwvk
