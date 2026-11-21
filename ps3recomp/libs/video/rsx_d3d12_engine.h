/* rsx_d3d12_engine -- "Direct3D 12 (experimental)": a D3D12 backend for the
 * register-file draw engine (rsx_draw_engine.c), the same engine the native
 * Vulkan renderer drives. Windows only. Selected with R2_RSX=d3d12.
 *
 * Environment:
 *   RSX_D3D12_DEBUG=1          enable the D3D12 debug layer and DRED (breadcrumbs +
 *                              page faults reported to stderr on device removal)
 *   R2_SHADER_MODE=sync        compile shaders on the render thread (default async:
 *                              worker pool, draws wait for their pipeline)
 *   RSX_D3D12_ASYNC=0|1        overrides R2_SHADER_MODE; RSX_D3D12_PIPELINE_THREADS=n
 *   RSX_D3D12_PSO_CACHE=0      do not persist PSOs (cache\d3d12_pso.bin)
 *   RSX_D3D12_FLIP_FRONT=1     invert the front-face winding (unverified convention)
 *   RSX_D3D12_DEPTH_RANGE=0    ignore the guest's depth range (viewport MinDepth/MaxDepth 0..1)
 *   RSX_D3D12_NV_D24_HALF=1    half-ULP depth bias for D24 targets on NVIDIA (off by default)
 *   RSX_D3D12_DEPTH_LEGACY=1   every depth target D24S8 (no D16 / D32F classes)
 *   RSX_ENG_POLYGON_MODE=1     honour the guest's wireframe fill mode
 *   RSX_D3D12_DUMP=<prefix>    write <prefix>NNNNNN.ppm of presented frames
 *   RSX_D3D12_DUMP_EVERY=<n>   ... every n-th present (default 60)
 *   RSX_D3D12_FAIL_DUMP=<dir>  write HLSL that FXC rejected into <dir>
 *   RSX_D3D12_ADAPTER=<n>      DXGI adapter index (default: the first hardware one)
 *   RSX_D3D12_GS=0             no point-size / point-sprite / wide-line expansion geometry shader
 *   RSX_D3D12_LOGIC_OP=0       ignore logic ops (default: CLEAR/COPY/NOOP exact, AND/OR & co. as blends;
 *                              XOR/NAND/INVERT & co. drawn unblended with a warning -- D3D12 logic ops
 *                              need UINT targets)
 *   PORT_OSD=layered           port UI as layered windows (default: drawn into the swap chain image)
 *   RSX_D3D12_NATIVE_16BIT=0   R5G6B5 / A1R5G5B5 surfaces as RGBA8 targets (default: B5G6R5 / B5G5R5A1)
 *   RSX_D3D12_NATIVE_8BIT=0    B8 / G8B8 surfaces as RGBA8 targets (default: R8 / R8G8, unverified)
 *   RSX_D3D12_HW_MSAA=1        RPCS3 "MSAA: Auto" on real multisampled twins (2x / 4x, default off,
 *                              unverified; not with RSX_UPSCALE < 1). Alpha-to-coverage then runs in
 *                              hardware on every pipeline; shading is per pixel, not per sample.
 *   RSX_UPSCALE=<0.25..1>      render scale for DLSS / FSR 3.1 / XeSS (corner window, unverified);
 *   RSX_D3D12_UPSCALE=0        keep 1:1 regardless; RSX_D3D12_UPSCALE_MIPBIAS=<f> texture LOD bias
 *   RSX_SHADERCACHE=0          do not cache compiled DXBC (shadercache\d3d12\)
 *   RSX_VSYNC=0|1|2            off (tearing when allowed) / on / adaptive (as on)
 *   PS3RECOMP_VK_OUTPUT_SCALING / RSX_FILTER = point|linear  present filter
 */
#ifndef RSX_D3D12_ENGINE_H
#define RSX_D3D12_ENGINE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when the renderer can start here: the full device bring-up (feature level
 * 11_0, heaps, root signatures, the internal shaders through FXC) and a swap chain on a
 * hidden window are created and torn down again. Cached after the first call. */
int  rsx_d3d12_engine_probe(void);
/* Late fallback when init fails after all: restart this executable with
 * R2_RSX=rpcs3 (same command line, once -- R2_D3D12_FALLBACK_RELAUNCH guards
 * loops). 1: the new process is running and the caller should exit. */
int  rsx_d3d12_engine_relaunch_compatible(void);
/* Every switch from D3D12 to the RPCS3 renderer: R2_RSX=rpcs3 and RSX_FSR in the RPCS3 renderer's
 * numbering (rsx_d3d12_fsr_to_rpcs3; FSR1 becomes RSX_FILTER=fsr unless RSX_FILTER is set). */
void rsx_d3d12_env_to_rpcs3(void);
/* D3D12 / native RSX_FSR -> RPCS3 RSX_FSR; *fsr1_filter = 1 for FSR1 (no RPCS3 RSX_FSR value). */
int  rsx_d3d12_fsr_to_rpcs3(int fsr, int* fsr1_filter);
/* The window to present into (an HWND); call before init. */
void rsx_d3d12_engine_set_window(void* hwnd);
/* Create the device and register the engine backend. 0 on success. */
int  rsx_d3d12_engine_init(uint32_t width, uint32_t height, const char* title);
void rsx_d3d12_engine_present(void);
void rsx_d3d12_engine_shutdown(void);
/* Diagnostics: the engine's draw count and the presented centre pixel. */
uint32_t rsx_d3d12_engine_guest_draws(void);
uint32_t rsx_d3d12_engine_presented_center(void);

/* Temporal upscaler for the engine's scene_aa hook (native AA at 1:1, the
 * game hooks of project/r3_native_aa.cpp drive it). kind: 1 NVIDIA DLSS
 * (r2_dlss.dll's D3D12 exports), 2 AMD FSR 3.1 (amd_fidelityfx_loader_dx12.dll),
 * 3 Intel XeSS (libxess.dll); sharpness 0..1 (FSR 3.1's RCAS); dlss_preset
 * as RSX_DLSS_PRESET. Call before init; 1 when the provider library loaded. */
int  rsx_d3d12_engine_use_upscaler(int kind, float sharpness, unsigned dlss_preset);
/* 1 once the device is up and the provider initialised (and has not failed). */
int  rsx_d3d12_engine_temporal_upscaler_ready(void);
/* Output scaling FSR1 (EASU + RCAS with RSX_SHARPEN) whenever the presented
 * image is upscaled, as RSX_FILTER=fsr. */
void rsx_d3d12_engine_force_output_fsr1(void);

/* Render scale < 1 for the temporal upscaler (RSX_UPSCALE; experimental, unverified in game):
 * set_render_scale before init, after use_upscaler succeeded (u in [0.25, 1), else off;
 * RSX_D3D12_UPSCALE=0 keeps 1:1). The game hooks open the scene window at each scene draw
 * (aa_window(1, scene colour handle)): draws into targets of the scene colour's size render
 * into their top-left corner at u x the size, and scene_aa upscales the scene colour to full
 * size. aa_window(0, 0) (or scene_aa / a present) closes it: every other target drawn
 * meanwhile is stretched back to full size. Returns the active scale (1 = off). */
float rsx_d3d12_engine_set_render_scale(float u);
void  rsx_d3d12_engine_aa_window(int open, unsigned color_handle);

/* HDR output (R3_HDR / R2_HDR / PORT_HDR = 0 | auto | 1 hdr10 | 2 scrgb, *_HDR_PAPER, *_HDR_PEAK,
 * *_HDR_SAT, as the Vulkan renderer): 0 SDR, 1 HDR10, 2 scRGB on the current swap chain. */
int rsx_d3d12_engine_hdr_active(void);
/* 1 while FSR 3 frame generation (rsx_d3d12_engine_fg.inc) adds a generated frame per present */
int rsx_d3d12_engine_fg_active(void);

/* Port UI in the swap chain (rsx_d3d12_engine_osd.inc): the r2rsx_osd_* calls of
 * project/port_osd.h, forwarded by rpcs3_bridge/r2rsx_osd.cpp while this renderer runs.
 * rsx_d3d12_engine_osd_api: NULL when D3D12 is not presenting (or PORT_OSD=layered). */
typedef struct rsx_d3d12_osd_api {
    int  (*wanted)(void);
    int  (*ready)(void);
    void (*layer)(int slot, const void* bgra, int w, int h, int x, int y, int ref_w, int ref_h, float alpha);
    void (*hide)(int slot);
    int  (*visible)(int slot);
    void (*suppress)(int on);
    unsigned (*presents)(void);
    void (*refresh)(void);
} rsx_d3d12_osd_api;
const rsx_d3d12_osd_api* rsx_d3d12_engine_osd_api(void);
/* Screenshot of the next presented frame without the port UI (the SDR image under HDR):
 * BGRA, w * 4 pitch, opaque; free with rsx_d3d12_engine_capture_free. 0 when D3D12 is not
 * presenting or no frame came within timeout_ms (do not call from the render thread). */
int  rsx_d3d12_engine_capture(void** bgra, int* w, int* h, unsigned timeout_ms);
void rsx_d3d12_engine_capture_free(void* p);
/* 1 while this renderer presents (rsx_d3d12_engine_capture can be asked from another thread). */
int  rsx_d3d12_engine_capture_available(void);
/* Present thread (main.cpp frame_clock), every loop: when the port UI changed (or a capture
 * waits) and nothing was presented for idle_ms, present the last frame again with the current
 * UI (no frame generation). Only while `parked` (the port paused the game, or no guest flip
 * yet) or a capture is pending -- a running game could be mid-frame. 1 when it presented. */
int  rsx_d3d12_engine_osd_repost(unsigned idle_ms, int parked);

/* Latency reduction (PORT_LATENCY + <game>_REFLEX, port_latency.h): NVIDIA Reflex through NVAPI
 * or AMD Anti-Lag 2 through the AMD D3D12 driver extension.
 * latency_frame: game thread, once per game frame before input is read (sleep / delay +
 *   simulation markers); 1 = Reflex markers are on, so the caller should route the first pad
 *   read of the frame to latency_input (INPUT_SAMPLE).
 * latency_kind: 0 none, 1 Reflex, 2 Anti-Lag 2. */
int  rsx_d3d12_engine_latency_frame(void);
void rsx_d3d12_engine_latency_input(void);
int  rsx_d3d12_engine_latency_kind(void);

/* Pure helpers (no device), exported for unit tests. */
/* GL blend factor -> D3D12_BLEND; `alpha` selects the alpha-channel rule
 * (D3D12 rejects *_COLOR factors there). */
int  rsx_d3d12_blend_factor(uint32_t gl, int alpha);
/* GL blend equation -> D3D12_BLEND_OP. */
int  rsx_d3d12_blend_op(uint32_t gl);
/* GL compare function (0x200..0x207, or 0..7) -> D3D12_COMPARISON_FUNC. */
int  rsx_d3d12_compare(uint32_t gl);
/* GL stencil op -> D3D12_STENCIL_OP. */
int  rsx_d3d12_stencil_op(uint32_t gl);
/* RSX wrap code (1..8) -> D3D12_TEXTURE_ADDRESS_MODE. */
int  rsx_d3d12_wrap(uint32_t code);
/* Letterbox: the destination rectangle of a src_w x src_h image with aspect
 * aw:ah (0 = stretch) inside a W x H window. */
void rsx_d3d12_letterbox(uint32_t W, uint32_t H, uint32_t aw, uint32_t ah,
                         uint32_t* x, uint32_t* y, uint32_t* w, uint32_t* h);
/* Proportional clip of a blit (src rect onto dst rect) against both extents.
 * Returns 0 when nothing is left. */
int  rsx_d3d12_clip_blit(uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h,
                         uint32_t* sx, uint32_t* sy, uint32_t* sw, uint32_t* sh,
                         uint32_t* dx, uint32_t* dy, uint32_t* dw, uint32_t* dh);
/* Point / wide-line expansion GS (rsx_d3d12_engine_ext.inc): the I/O part
 * (VSOutput, GSOut in the PS's PSInput layout, gs_copy) into out; its length,
 * 0 when the shader pair cannot be expanded. */
size_t rsx_d3d12_gs_io(const char* vs_hlsl, const char* ps_hlsl, char* out, size_t cap);
/* The GS entry point "main" following gs_io: kind 1 points, 2 lines. */
const char* rsx_d3d12_gs_body(int kind);
/* PS rewritten to read the GS's sprite coordinate (malloc'd), or NULL. */
char* rsx_d3d12_ps_pcoord_fix(const char* ps_hlsl);
/* Blend standing in for GL logic op `op` on a UNORM target (a
 * D3D12_RENDER_TARGET_BLEND_DESC*): 1 exact, 2 approximate, 0 unsupported. */
struct D3D12_RENDER_TARGET_BLEND_DESC;
int  rsx_d3d12_logic_blend(uint32_t op, struct D3D12_RENDER_TARGET_BLEND_DESC* t);
/* Render scale (rsx_d3d12_engine_ext.inc): RSX_UPSCALE parsed (1 = off); a guest extent /
 * rectangle at scale u; jitter phases 8 x (1/u)^2 (8..64); texture LOD bias log2(u) in
 * quarter steps; XeSS quality setting whose render size covers output / ratio; the PS with its 2D texture
 * coordinates scaled by cbuffer D3EUp (b3) d3e_uvk[unit] (malloc'd, NULL: no samplers). */
float    rsx_d3d12_up_parse(const char* s);
uint32_t rsx_d3d12_up_dim(uint32_t v, float u);
void     rsx_d3d12_up_rect(float u, uint32_t* x, uint32_t* y, uint32_t* w, uint32_t* h);
uint32_t rsx_d3d12_up_phases(float u);
uint32_t rsx_d3d12_ms_sample(uint32_t sx, uint32_t sy, uint32_t x, uint32_t y);
void     rsx_d3d12_ms_rect(uint32_t sx, uint32_t sy, uint32_t* x, uint32_t* y, uint32_t* w, uint32_t* h);
int      rsx_d3d12_ms_samples(uint32_t sx, uint32_t sy, uint32_t* samples);
float    rsx_d3d12_up_mip_bias(float u);
int      rsx_d3d12_up_xess_quality(float ratio);
char*    rsx_d3d12_ps_uv_scale(const char* ps_hlsl);
/* R5G6B5 (kind 1) / A1R5G5B5 (kind 2) / R8 = B8 (kind 3) / R8G8 = G8B8 (kind 4)
 * texels <-> the RGBA8 lanes, and the lane mapping / clear colour / write mask. */
void rsx_d3d12_cv_to_rgba(int kind, const uint8_t* src16, uint8_t* rgba, uint32_t n);
void rsx_d3d12_cv_from_rgba(int kind, const uint8_t* rgba, uint8_t* dst16, uint32_t n);
uint32_t rsx_d3d12_cv_map(int kind, uint32_t d3d12_map);
void rsx_d3d12_cv_clear(int kind, const float in[4], float out[4]);
uint32_t rsx_d3d12_cv_write_mask(int kind, uint32_t mask);
uint32_t rsx_d3d12_cv_bpp(int kind);

#ifdef __cplusplus
}
#endif

#endif
