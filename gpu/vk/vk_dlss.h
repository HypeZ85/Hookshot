// NVIDIA DLSS Super Resolution, one of the temporal upscaler's engines (vk_temporal.h; vk_dlss.cpp). Built in only with
// NVIDIA's NGX SDK (HOOKSHOT_NGX, scripts/ngx_sdk.py: Linux and Windows); without it these say DLSS is not there and the
// game's network runs.
#pragma once

#include <vector>
#include "dlss_nr_support.h"
#include "dlss_nr_settings.h"

#include "vk_internal.h"

namespace mkwvk {

struct TemporalEngine;

/// Whether this build has DLSS in it (HOOKSHOT_NGX)
bool dlss_built();
enum DlssNrStatus { NR_WAITING, NR_ACTIVE, NR_PENDING, NR_FAILED };
DlssNrStatus dlss_nr_status(); // Last NR evaluation, for the live menu (never inferred from the enable switch).
dlss_nr::Issue dlss_nr_check(bool refresh = false); // Checks local runtime, selected GPU, upstream and NGX capability.
dlss_nr::Issue dlss_nr_issue();                   // Persistent, non-modal diagnostic, independent of requested enable.
void dlss_nr_retry();
void dlss_nr_compare(int override_value);         // -1 saved choice, 0 bypass, 1 request NR (never saves).
struct DlssNrControls {
    bool requested = false;
    dlss_nr::Settings model;
    int color = 100, history = 0, debug = 0;
    bool stats = false;
    bool native_hdr = false;
};
void dlss_nr_publish(const DlssNrControls& controls); // Immutable per-frame snapshot, separate from persisted UI options.
struct DlssNrStats {
    double gpu_ms = -1;
    uint64_t images_bytes = 0, frames = 0;
    uint32_t width = 0, height = 0;
    bool active = false;
    char runtime[48]{};
};
DlssNrStats dlss_nr_stats();
/// Why DLSS cannot be chosen here (the Insert menu's Upscaler row says it), DLSS_OK when it can or is not known yet
enum DlssWhy { DLSS_OK, DLSS_NOT_BUILT, DLSS_ENV_OFF, DLSS_NEEDS_RTX, DLSS_NEEDS_DRIVER, DLSS_NO_RUNTIME,
               DLSS_RUNTIME_NOT_ASCII, DLSS_WHY_COUNT };
DlssWhy dlss_why();
/// Asks NGX once whether DLSS runs on this device, so dlss_why knows before the first upscale (the menu, when it opens).
/// Nothing off an NVIDIA device with NGX's extensions, or in a build without DLSS
void dlss_probe();
/// The instance's extensions DLSS needs, added to `want` when the loader has them (all: what it has)
void dlss_instance_extensions(std::vector<const char*>& want, const std::vector<VkExtensionProperties>& all);
/// The device's extensions DLSS needs, added to `want` when the device has them
void dlss_device_extensions(std::vector<const char*>& want, const std::vector<VkExtensionProperties>& all);
/// DLSS as the temporal upscaler's engine (DLSS and DLAA on the Upscaler row); nullptr in a build without it
const TemporalEngine* dlss_engine();

} // namespace mkwvk
