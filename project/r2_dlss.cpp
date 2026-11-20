// r2_dlss.dll: the only part of the Resistance 2 port that uses the NVIDIA DLSS (NGX) SDK, Vulkan flavour.
// Loaded at run time by the RPCS3 Vulkan renderer (RSX_FSR=2, VKR2Upscaler.cpp: the r2_dlss_* exports) and
// by ps3recomp's native Vulkan renderer (rsx_vk_upscale.c: port_dlss_vk_get_api, the port-neutral table in
// ps3recomp/libs/video/rsx_vk_dlss_api.h). Without it, or on a non-RTX GPU, both fall back to FSR 3.1.
// Other ports can build this file under their own name and set their NGX project id (set_project).
// Also NGX's D3D12 flavour (r2_dlss_d3d12_*) for the Direct3D 12 (experimental) renderer (rsx_d3d12_engine_aa.inc).
// Built by build.bat: clang-cl /MT /LD r2_dlss.cpp tools\dlss\lib\nvsdk_ngx_s.lib

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <vulkan/vulkan.h>
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_vk.h>

#include "../ps3recomp/libs/video/rsx_vk_dlss_api.h"
#include "../ps3recomp/libs/video/port_dlss_quality.h"
#pragma comment(lib, "version.lib")   // GetFileVersionInfoW: the nvngx_dlss.dll version log

typedef void (*R2DlssLog)(const char* msg);

namespace {
std::string g_project_id = "c4a7e2d1-3b58-4f6a-8e92-71d5b0c3f6e8";
std::string g_engine_version = "Resistance2-ps3recomp";

R2DlssLog g_log;
VkDevice g_dev;
bool g_init;
NVSDK_NGX_Parameter* g_caps;
NVSDK_NGX_Parameter* g_params;
NVSDK_NGX_Handle* g_feature;
unsigned g_w, g_h;
std::wstring g_dir;

void Log(const char* fmt, ...) {
    if (!g_log) return;
    char buf[1024];
    va_list a; va_start(a, fmt); vsnprintf(buf, sizeof buf, fmt, a); va_end(a);
    g_log(buf);
}
bool Ok(const char* what, NVSDK_NGX_Result r) {
    if (NVSDK_NGX_FAILED(r)) { Log("%s failed: 0x%08x", what, (unsigned)r); return false; }
    return true;
}
NVSDK_NGX_Resource_VK Res(VkImage img, VkImageView view, int fmt, unsigned w, unsigned h, bool depth, bool rw) {
    VkImageSubresourceRange range = { depth ? (VkImageAspectFlags)VK_IMAGE_ASPECT_DEPTH_BIT : (VkImageAspectFlags)VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    return NVSDK_NGX_Create_ImageView_Resource_VK(view, img, range, (VkFormat)fmt, w, h, rw);
}

// The nvngx_dlss.dll NGX actually loaded (an NVIDIA App / driver override can replace the one next to the game)
// and its file version. false: not found / no version resource.
bool DlssDllVersion(unsigned v[4], std::wstring& path) {
    wchar_t buf[MAX_PATH] = {};
    HMODULE m = GetModuleHandleW(L"nvngx_dlss.dll");
    if (m && GetModuleFileNameW(m, buf, MAX_PATH)) path = buf;
    else path = g_dir + L"\\nvngx_dlss.dll";
    DWORD hnd = 0;
    const DWORD n = GetFileVersionInfoSizeW(path.c_str(), &hnd);
    if (!n) return false;
    std::vector<char> data(n);
    VS_FIXEDFILEINFO* fi = nullptr;
    UINT len = 0;
    if (!GetFileVersionInfoW(path.c_str(), 0, n, data.data()) ||
        !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&fi), &len) || !fi || len < sizeof(*fi))
        return false;
    v[0] = HIWORD(fi->dwFileVersionMS); v[1] = LOWORD(fi->dwFileVersionMS);
    v[2] = HIWORD(fi->dwFileVersionLS); v[3] = LOWORD(fi->dwFileVersionLS);
    return true;
}

// Preset / DLL logging (always on, log only). NGX has no "current preset" query, so this logs the requested
// preset and the DLL version, and warns once when that DLL does not honour it (pdq_preset_check).
void LogDllAndPreset(unsigned preset, const char* mode) {
    static bool dll_logged;
    static bool warned;
    unsigned v[4] = {};
    std::wstring path;
    const bool have = DlssDllVersion(v, path);
    if (!dll_logged) {
        dll_logged = true;
        char p8[MAX_PATH * 3] = {};
        WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, p8, sizeof p8, nullptr, nullptr);
        if (have) Log("nvngx_dlss.dll %u.%u.%u.%u (%s)", v[0], v[1], v[2], v[3], p8);
        else Log("nvngx_dlss.dll version unknown (%s)", p8);
        unsigned opt = 0, dev = 0;
        if (g_params && NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetUI(g_params, NVSDK_NGX_Parameter_OptLevel, &opt))) {
            NVSDK_NGX_Parameter_GetUI(g_params, NVSDK_NGX_Parameter_IsDevSnippetBranch, &dev);
            Log("DLSS snippet opt level %u%s", opt, dev ? " (dev branch)" : "");
        }
    }
    const int pc = have ? pdq_preset_check(v[0], v[1], preset) : PDQ_PRESET_OK;
    Log("DLSS preset requested for %s: %s%c (NGX has no applied-preset query; %s)", mode, preset ? "" : "default", preset ? pdq_preset_letter(preset) : ' ',
        pc == PDQ_PRESET_OK ? "this DLL supports it" : pc == PDQ_PRESET_DEPRECATED ? "deprecated in this DLL" :
        pc == PDQ_PRESET_REMOVED ? "removed in this DLL: NVIDIA's default is used" : "this DLL predates it: NVIDIA's default is used");
    if ((preset == 5 || preset == 6) && pc != PDQ_PRESET_OK && !warned) {
        warned = true;
        Log("WARNING: preset %c was requested but nvngx_dlss.dll %u.%u %s; check the NVIDIA overlay (DLSS indicator) for the preset in use",
            pdq_preset_letter(preset), v[0], v[1], pc == PDQ_PRESET_DEPRECATED ? "deprecates it (DLSS 4 DLLs may ignore or remap E/F)" : "ignores it");
    }
}
} // namespace

extern "C" {

// Vulkan extensions NGX needs; the renderer enables the supported ones when RSX_FSR=2.
__declspec(dllexport) int r2_dlss_required_extensions(unsigned* inst_count, const char*** inst_exts,
                                                      unsigned* dev_count, const char*** dev_exts)
{
    return NVSDK_NGX_SUCCEED(NVSDK_NGX_VULKAN_RequiredExtensions(inst_count, inst_exts, dev_count, dev_exts)) ? 1 : 0;
}

// dir: folder holding nvngx_dlss.dll (and where NGX may write its logs). 1 = DLSS usable.
__declspec(dllexport) int r2_dlss_init(VkInstance inst, VkPhysicalDevice pdev, VkDevice dev,
                                       PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr gdpa,
                                       const wchar_t* dir, R2DlssLog log)
{
    g_log = log;
    if (g_init) return 1;
    if (!inst || !pdev || !dev) return 0;
    g_dir = dir ? dir : L".";
    static const wchar_t* paths[1];
    paths[0] = g_dir.c_str();
    NVSDK_NGX_FeatureCommonInfo common = {};
    common.PathListInfo.Path = paths;
    common.PathListInfo.Length = 1;
    common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;
    if (!Ok("NGX init", NVSDK_NGX_VULKAN_Init_with_ProjectID(g_project_id.c_str(), NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                             g_engine_version.c_str(),
                                                             g_dir.c_str(), inst, pdev, dev, gipa, gdpa, &common,
                                                             NVSDK_NGX_Version_API)))
        return 0;
    g_init = true; g_dev = dev;
    if (!Ok("NGX capability query", NVSDK_NGX_VULKAN_GetCapabilityParameters(&g_caps))) return 0;
    int avail = 0, drv = 0;
    NVSDK_NGX_Parameter_GetI(g_caps, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &drv);
    NVSDK_NGX_Parameter_GetI(g_caps, NVSDK_NGX_Parameter_SuperSampling_Available, &avail);
    if (drv) { Log("DLSS needs a newer NVIDIA driver"); return 0; }
    if (!avail) { Log("DLSS is not available on this GPU"); return 0; }
    return 1;
}

__declspec(dllexport) void r2_dlss_release(void)
{
    if (g_feature) { NVSDK_NGX_VULKAN_ReleaseFeature(g_feature); g_feature = nullptr; }
    if (g_params) { NVSDK_NGX_VULKAN_DestroyParameters(g_params); g_params = nullptr; }
}

// Records feature creation into cmd. flags: 1 depth inverted, 2 HDR (+auto exposure).
// preset: NVSDK_NGX_DLSS_Hint_Render_Preset (0 = NVIDIA's default).
__declspec(dllexport) int r2_dlss_create(VkCommandBuffer cmd, unsigned w, unsigned h,
                                         unsigned ow, unsigned oh, int flags, unsigned preset)
{
    if (!g_init || !g_caps) return 0;
    r2_dlss_release();
    if (!Ok("NGX parameter allocation", NVSDK_NGX_VULKAN_AllocateParameters(&g_params))) return 0;
    const char* keys[] = { NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality };
    for (const char* k : keys) NVSDK_NGX_Parameter_SetUI(g_params, k, preset);
    NVSDK_NGX_DLSS_Create_Params cp = {};
    cp.Feature.InWidth = w; cp.Feature.InHeight = h;
    cp.Feature.InTargetWidth = ow; cp.Feature.InTargetHeight = oh;
    const float r = ow ? (float)w / (float)ow : 1.0f;
    const NVSDK_NGX_PerfQuality_Value q = r > 0.99f ? NVSDK_NGX_PerfQuality_Value_DLAA
                                        : r > 0.62f ? NVSDK_NGX_PerfQuality_Value_MaxQuality
                                        : r > 0.54f ? NVSDK_NGX_PerfQuality_Value_Balanced
                                        : r > 0.42f ? NVSDK_NGX_PerfQuality_Value_MaxPerf
                                                    : NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    static const char* qn[] = { "Performance", "Balanced", "Quality", "Ultra Performance", "Ultra Quality", "DLAA" };
    cp.Feature.InPerfQualityValue = q;
    cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                              ((flags & 1) ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0) |
                              ((flags & 2) ? (NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure) : 0);
    if (!Ok("DLSS feature creation", NGX_VULKAN_CREATE_DLSS_EXT1(g_dev, cmd, 1, 1, &g_feature, g_params, &cp)) || !g_feature) {
        g_feature = nullptr; return 0;
    }
    g_w = w; g_h = h;
    Log("DLSS feature %ux%u -> %ux%u (%s), preset %u", w, h, ow, oh, (unsigned)q < 6 ? qn[q] : "?", preset);
    LogDllAndPreset(preset, (unsigned)q < 6 ? qn[q] : "?");
    return 1;
}

// Optional R8 mask (render resolution) for the next evaluations: 1 = favour the current frame
// (moving objects the camera-only motion vectors miss). image = VK_NULL_HANDLE turns it off.
static VkImage g_mask = VK_NULL_HANDLE;
static VkImageView g_mask_view = VK_NULL_HANDLE;
static int g_mask_fmt = 0;
__declspec(dllexport) void r2_dlss_set_mask(VkImage mask, VkImageView mask_view, int mask_fmt)
{
    g_mask = mask; g_mask_view = mask_view; g_mask_fmt = mask_fmt;
}

// Inputs in SHADER_READ_ONLY_OPTIMAL, output in GENERAL. Formats are VkFormat values.
// jitter: render pixels; mv * mv_scale = render pixels, current -> previous.
__declspec(dllexport) int r2_dlss_eval(VkCommandBuffer cmd,
                                       VkImage color, VkImageView color_view, int color_fmt,
                                       VkImage depth, VkImageView depth_view, int depth_fmt,
                                       VkImage mv, VkImageView mv_view, int mv_fmt,
                                       VkImage out, VkImageView out_view, int out_fmt,
                                       unsigned out_w, unsigned out_h,
                                       float jx, float jy, float mvsx, float mvsy, int reset, float frame_ms)
{
    if (!g_feature || !g_params) return 0;
    NVSDK_NGX_Resource_VK rc = Res(color, color_view, color_fmt, g_w, g_h, false, false);
    NVSDK_NGX_Resource_VK rd = Res(depth, depth_view, depth_fmt, g_w, g_h, false, false);
    NVSDK_NGX_Resource_VK rm = Res(mv, mv_view, mv_fmt, g_w, g_h, false, false);
    NVSDK_NGX_Resource_VK ro = Res(out, out_view, out_fmt, out_w, out_h, false, true);
    NVSDK_NGX_VK_DLSS_Eval_Params ep = {};
    ep.Feature.pInColor = &rc;
    ep.Feature.pInOutput = &ro;
    ep.pInDepth = &rd;
    ep.pInMotionVectors = &rm;
    ep.InJitterOffsetX = jx; ep.InJitterOffsetY = jy;
    ep.InRenderSubrectDimensions.Width = g_w; ep.InRenderSubrectDimensions.Height = g_h;
    ep.InReset = reset;
    ep.InMVScaleX = mvsx; ep.InMVScaleY = mvsy;
    ep.InPreExposure = 1.0f; ep.InExposureScale = 1.0f;
    ep.InFrameTimeDeltaInMsec = frame_ms;
    NVSDK_NGX_Resource_VK rb;
    if (g_mask && g_mask_view) {
        rb = Res(g_mask, g_mask_view, g_mask_fmt, g_w, g_h, false, false);
        ep.pInBiasCurrentColorMask = &rb;
    }
    return Ok("DLSS evaluation", NGX_VULKAN_EVALUATE_DLSS_EXT(cmd, g_feature, g_params, &ep)) ? 1 : 0;
}

__declspec(dllexport) void r2_dlss_shutdown(void)
{
    r2_dlss_release();
    if (g_caps) { NVSDK_NGX_VULKAN_DestroyParameters(g_caps); g_caps = nullptr; }
    if (g_init) { NVSDK_NGX_VULKAN_Shutdown1(g_dev); g_init = false; g_dev = VK_NULL_HANDLE; }
}

} // extern "C"

// ---- port_dlss_vk_get_api (rsx_vk_dlss_api.h) --------------------------------
namespace {

void P_set_project(const char* project_id, const char* engine_version)
{
    if (project_id && *project_id) g_project_id = project_id;
    if (engine_version && *engine_version) g_engine_version = engine_version;
}

std::wstring exe_dir()
{
    wchar_t p[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH);
    while (n && p[n - 1] != L'\\' && p[n - 1] != L'/') n--;
    return n ? std::wstring(p, n - 1) : std::wstring(L".");
}

// Name lists handed out (static storage until the next call).
std::vector<std::string> g_inst_names, g_dev_names;
std::vector<const char*> g_inst_ptrs, g_dev_ptrs;

void add_unique(std::vector<std::string>& v, const char* s)
{
    if (!s || !*s) return;
    for (const auto& x : v) if (x == s) return;
    v.emplace_back(s);
}

int P_required_extensions(void* instance, void* phys, unsigned* ni, const char*** ie, unsigned* nd, const char*** de)
{
    g_inst_names.clear(); g_dev_names.clear();
    unsigned li = 0, ld = 0;
    const char** lie = nullptr; const char** lde = nullptr;
    const bool legacy = NVSDK_NGX_SUCCEED(NVSDK_NGX_VULKAN_RequiredExtensions(&li, &lie, &ld, &lde));
    if (legacy) {
        for (unsigned i = 0; i < li; i++) add_unique(g_inst_names, lie[i]);
        for (unsigned i = 0; i < ld; i++) add_unique(g_dev_names, lde[i]);
    }
    // DLSS-specific lists (NGX 3.x): they need no NGX init, the device one needs the instance and GPU.
    static std::wstring dir;
    dir = exe_dir();
    NVSDK_NGX_FeatureCommonInfo common = {};
    static const wchar_t* paths[1];
    paths[0] = dir.c_str();
    common.PathListInfo.Path = paths;
    common.PathListInfo.Length = 1;
    NVSDK_NGX_FeatureDiscoveryInfo fi = {};
    fi.SDKVersion = NVSDK_NGX_Version_API;
    fi.FeatureID = NVSDK_NGX_Feature_SuperSampling;
    fi.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    fi.Identifier.v.ProjectDesc.ProjectId = g_project_id.c_str();
    fi.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    fi.Identifier.v.ProjectDesc.EngineVersion = g_engine_version.c_str();
    fi.ApplicationDataPath = dir.c_str();
    fi.FeatureInfo = &common;
    bool feature = false;
    uint32_t n = 0;
    VkExtensionProperties* props = nullptr;
    if (NVSDK_NGX_SUCCEED(NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&fi, &n, &props)) && props) {
        for (uint32_t i = 0; i < n; i++) add_unique(g_inst_names, props[i].extensionName);
        feature = true;
    }
    if (instance && phys) {
        n = 0; props = nullptr;
        if (NVSDK_NGX_SUCCEED(NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
                (VkInstance)instance, (VkPhysicalDevice)phys, &fi, &n, &props)) && props) {
            for (uint32_t i = 0; i < n; i++) add_unique(g_dev_names, props[i].extensionName);
            feature = true;
        }
    }
    if (!legacy && !feature) return 0;
    g_inst_ptrs.clear(); g_dev_ptrs.clear();
    for (const auto& s : g_inst_names) g_inst_ptrs.push_back(s.c_str());
    for (const auto& s : g_dev_names) g_dev_ptrs.push_back(s.c_str());
    *ni = (unsigned)g_inst_ptrs.size(); *ie = g_inst_ptrs.empty() ? nullptr : g_inst_ptrs.data();
    *nd = (unsigned)g_dev_ptrs.size(); *de = g_dev_ptrs.empty() ? nullptr : g_dev_ptrs.data();
    return 1;
}

int P_init(void* inst, void* phys, void* dev, void* gipa, void* gdpa, const wchar_t* dir, port_dlss_vk_log_fn log)
{
    return r2_dlss_init((VkInstance)inst, (VkPhysicalDevice)phys, (VkDevice)dev, (PFN_vkGetInstanceProcAddr)gipa,
                        (PFN_vkGetDeviceProcAddr)gdpa, dir, log);
}

int P_create(void* cmd, unsigned w, unsigned h, unsigned ow, unsigned oh, int flags, unsigned preset)
{
    return r2_dlss_create((VkCommandBuffer)cmd, w, h, ow, oh, flags, preset);
}

int P_evaluate(void* cmd, const port_dlss_vk_eval* e)
{
    if (!e) return 0;
    r2_dlss_set_mask((VkImage)(uintptr_t)e->mask.image, (VkImageView)(uintptr_t)e->mask.view, e->mask.format);
    const int ok = r2_dlss_eval((VkCommandBuffer)cmd,
                                (VkImage)(uintptr_t)e->color.image, (VkImageView)(uintptr_t)e->color.view, e->color.format,
                                (VkImage)(uintptr_t)e->depth.image, (VkImageView)(uintptr_t)e->depth.view, e->depth.format,
                                (VkImage)(uintptr_t)e->motion.image, (VkImageView)(uintptr_t)e->motion.view, e->motion.format,
                                (VkImage)(uintptr_t)e->output.image, (VkImageView)(uintptr_t)e->output.view, e->output.format,
                                e->out_w, e->out_h, e->jitter_x, e->jitter_y, e->mv_scale_x, e->mv_scale_y,
                                e->reset, e->frame_ms);
    r2_dlss_set_mask(VK_NULL_HANDLE, VK_NULL_HANDLE, 0);
    return ok;
}

int P_optimal_render_size(unsigned ow, unsigned oh, int quality, unsigned* w, unsigned* h)
{
    if (!g_caps || !w || !h) return 0;
    unsigned mxw = 0, mxh = 0, mnw = 0, mnh = 0;
    float sharp = 0.f;
    const NVSDK_NGX_Result r = NGX_DLSS_GET_OPTIMAL_SETTINGS(g_caps, ow, oh, (NVSDK_NGX_PerfQuality_Value)quality,
                                                             w, h, &mxw, &mxh, &mnw, &mnh, &sharp);
    return NVSDK_NGX_SUCCEED(r) && *w && *h ? 1 : 0;
}

const port_dlss_vk_api g_api = {
    PORT_DLSS_VK_API_VERSION, (unsigned)sizeof(port_dlss_vk_api),
    P_set_project, P_required_extensions, P_init, P_create, P_evaluate, P_optimal_render_size,
    r2_dlss_release, r2_dlss_shutdown,
};

} // namespace

extern "C" __declspec(dllexport) const port_dlss_vk_api* port_dlss_vk_get_api(unsigned version)
{
    return version >= 1u && version <= PORT_DLSS_VK_API_VERSION ? &g_api : nullptr;
}

// ---- Direct3D 12 (the "Direct3D 12 (experimental)" renderer, rsx_d3d12_engine.c) ----------------------
// The same DLSS feature over NGX's D3D12 entry points; state separate from the Vulkan one above.
// Inputs (colour RGBA16F, depth R32F, motion RG16F, optional R8 mask) in a shader-resource state, output
// (RGBA16F, UAV) in UNORDERED_ACCESS. jitter: render pixels; mv * mv_scale = render pixels, current -> previous.
namespace {
ID3D12Device* g12_dev;
bool g12_init;
NVSDK_NGX_Parameter* g12_caps;
NVSDK_NGX_Parameter* g12_params;
NVSDK_NGX_Handle* g12_feature;
unsigned g12_w, g12_h;

void D12Release()
{
    if (g12_feature) { NVSDK_NGX_D3D12_ReleaseFeature(g12_feature); g12_feature = nullptr; }
    if (g12_params) { NVSDK_NGX_D3D12_DestroyParameters(g12_params); g12_params = nullptr; }
}
} // namespace

extern "C" {

__declspec(dllexport) int r2_dlss_d3d12_init(ID3D12Device* dev, const wchar_t* dir, R2DlssLog log)
{
    g_log = log;
    if (g12_init) return 1;
    if (!dev) return 0;
    g_dir = dir ? dir : L".";
    static const wchar_t* paths[1];
    paths[0] = g_dir.c_str();
    NVSDK_NGX_FeatureCommonInfo common = {};
    common.PathListInfo.Path = paths;
    common.PathListInfo.Length = 1;
    common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;
    if (!Ok("NGX D3D12 init", NVSDK_NGX_D3D12_Init_with_ProjectID(g_project_id.c_str(), NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                                  g_engine_version.c_str(), g_dir.c_str(), dev,
                                                                  &common, NVSDK_NGX_Version_API)))
        return 0;
    g12_init = true; g12_dev = dev;
    if (!Ok("NGX D3D12 capability query", NVSDK_NGX_D3D12_GetCapabilityParameters(&g12_caps))) return 0;
    int avail = 0, drv = 0;
    NVSDK_NGX_Parameter_GetI(g12_caps, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &drv);
    NVSDK_NGX_Parameter_GetI(g12_caps, NVSDK_NGX_Parameter_SuperSampling_Available, &avail);
    if (drv) { Log("DLSS needs a newer NVIDIA driver"); return 0; }
    if (!avail) { Log("DLSS is not available on this GPU"); return 0; }
    return 1;
}

// Records feature creation into cmd. flags: 1 depth inverted, 2 HDR (+auto exposure). preset as r2_dlss_create.
__declspec(dllexport) int r2_dlss_d3d12_create(ID3D12GraphicsCommandList* cmd, unsigned w, unsigned h,
                                               unsigned ow, unsigned oh, int flags, unsigned preset)
{
    if (!g12_init || !g12_caps || !cmd) return 0;
    D12Release();
    if (!Ok("NGX D3D12 parameter allocation", NVSDK_NGX_D3D12_AllocateParameters(&g12_params))) return 0;
    const char* keys[] = { NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality };
    for (const char* k : keys) NVSDK_NGX_Parameter_SetUI(g12_params, k, preset);
    NVSDK_NGX_DLSS_Create_Params cp = {};
    cp.Feature.InWidth = w; cp.Feature.InHeight = h;
    cp.Feature.InTargetWidth = ow; cp.Feature.InTargetHeight = oh;
    const float r = ow ? (float)w / (float)ow : 1.0f;
    const NVSDK_NGX_PerfQuality_Value q = r > 0.99f ? NVSDK_NGX_PerfQuality_Value_DLAA
                                        : r > 0.62f ? NVSDK_NGX_PerfQuality_Value_MaxQuality
                                        : r > 0.54f ? NVSDK_NGX_PerfQuality_Value_Balanced
                                        : r > 0.42f ? NVSDK_NGX_PerfQuality_Value_MaxPerf
                                                    : NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    static const char* qn[] = { "Performance", "Balanced", "Quality", "Ultra Performance", "Ultra Quality", "DLAA" };
    cp.Feature.InPerfQualityValue = q;
    cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                              ((flags & 1) ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0) |
                              ((flags & 2) ? (NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure) : 0);
    if (!Ok("DLSS D3D12 feature creation", NGX_D3D12_CREATE_DLSS_EXT(cmd, 1, 1, &g12_feature, g12_params, &cp)) ||
        !g12_feature) {
        g12_feature = nullptr;
        return 0;
    }
    g12_w = w; g12_h = h;
    Log("DLSS (D3D12) feature %ux%u -> %ux%u (%s), preset %u", w, h, ow, oh, (unsigned)q < 6 ? qn[q] : "?", preset);
    std::swap(g_params, g12_params);      // LogDllAndPreset reads g_params' snippet level
    LogDllAndPreset(preset, (unsigned)q < 6 ? qn[q] : "?");
    std::swap(g_params, g12_params);
    return 1;
}

__declspec(dllexport) int r2_dlss_d3d12_eval(ID3D12GraphicsCommandList* cmd, ID3D12Resource* color,
                                             ID3D12Resource* depth, ID3D12Resource* mv, ID3D12Resource* mask,
                                             ID3D12Resource* out, unsigned out_w, unsigned out_h,
                                             float jx, float jy, float mvsx, float mvsy, int reset, float frame_ms)
{
    (void)out_w; (void)out_h;
    if (!g12_feature || !g12_params || !cmd) return 0;
    NVSDK_NGX_D3D12_DLSS_Eval_Params ep = {};
    ep.Feature.pInColor = color;
    ep.Feature.pInOutput = out;
    ep.pInDepth = depth;
    ep.pInMotionVectors = mv;
    ep.pInBiasCurrentColorMask = mask;
    ep.InJitterOffsetX = jx; ep.InJitterOffsetY = jy;
    ep.InRenderSubrectDimensions.Width = g12_w; ep.InRenderSubrectDimensions.Height = g12_h;
    ep.InReset = reset;
    ep.InMVScaleX = mvsx; ep.InMVScaleY = mvsy;
    ep.InPreExposure = 1.0f; ep.InExposureScale = 1.0f;
    ep.InFrameTimeDeltaInMsec = frame_ms;
    return Ok("DLSS D3D12 evaluation", NGX_D3D12_EVALUATE_DLSS_EXT(cmd, g12_feature, g12_params, &ep)) ? 1 : 0;
}

__declspec(dllexport) void r2_dlss_d3d12_shutdown(void)
{
    D12Release();
    if (g12_caps) { NVSDK_NGX_D3D12_DestroyParameters(g12_caps); g12_caps = nullptr; }
    if (g12_init) { NVSDK_NGX_D3D12_Shutdown1(g12_dev); g12_init = false; g12_dev = nullptr; }
}

} // extern "C"