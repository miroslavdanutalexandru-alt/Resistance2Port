/*
 * ps3recomp game project -- entry point template.
 *
 * This is the runner. It boots a lifted title through the toolkit's PPU
 * scaffold: allocate the flat guest VM, load the PPU ELF named on the command
 * line, register the lifted function table and the HLE NID handlers, start a
 * frame clock, and dispatch the entry OPD.
 *
 * The scaffold does the work; nothing below reimplements it. ppu_load_elf,
 * ppu_recomp_register, ppu_hle_init, ppu_sysprx_register, ppu_fs_register,
 * lv2_init_syscalls and ppu_run all come from the toolkit, compiled into this
 * project from PS3RECOMP_DIR (see CMakeLists.txt). What is left here is the
 * part a port owns: which backend to present through, how the frame clock is
 * paced, and whatever diagnostics the title turns out to need.
 *
 * runtime/ppu/tests/boot_main.cpp in the toolkit is the same boot in its
 * fully-instrumented form -- crash filter, hang watchdog, guest-PC sampling
 * profiler. Read it when a boot goes wrong; copy from it what the title
 * needs. It is deliberately not what a fresh project starts with.
 *
 * The one part that IS duplicated from it is the frame clock below, because a
 * port is expected to change its pacing and a shared one would be the wrong
 * shape for that. It was copied faithfully, comments and all. If a pacing bug
 * is fixed in one of the two, look at the other.
 *
 * This builds on Windows, macOS and Linux. CI builds and runs it against the
 * boot smoke title on the last two, and compile-checks it against clang-cl on
 * the first (tools/check_ppu_scaffold.py).
 */

/* The lifter's generated header, which declares ppu_context and the function
 * table. It comes first: everything below is written against that struct. */
#include "ppu_recomp.h"

/* The Win32 names -- Sleep, GetTickCount64, CreateThread, InterlockedIncrement,
 * VirtualAlloc, the scalar typedefs -- from one place. On Windows this is a
 * passthrough to <windows.h>; off Windows the toolkit's shim supplies the same
 * names over pthreads and mmap, so the call sites below are written once. */
#include "win32_compat.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/mman.h>   /* mmap: the guest arena, see alloc_guest_vm */
#endif

#ifdef _WIN32
/* timeBeginPeriod: <windows.h> arrives with WIN32_LEAN_AND_MEAN set, which
 * excludes the multimedia timer API, so it is asked for by name -- and after
 * windows.h, since timeapi.h uses UINT. No shim exists off Windows because
 * there is nothing to shim: the 15.6 ms default timer granularity this works
 * around is a Windows problem. */
#include <timeapi.h>
#endif

/* ---------------------------------------------------------------------------
 * What the scaffold gives this file
 * -----------------------------------------------------------------------*/
extern "C" {
uint32_t ppu_load_elf(const char* path);      /* ELF -> guest RAM, returns entry OPD */
void     ppu_recomp_register(void);           /* generated: lifted table -> address map */
void     ppu_hle_init(void);                  /* firmware import NID -> HLE handler */
void     ppu_sysprx_register(void);           /* boot-critical CRT (sys_initialize_tls, ...) */
void     ppu_fs_register(void);               /* cellFs over the real game directory */
void     lv2_init_syscalls(void);             /* the lv2 syscall table */
int      ppu_run(uint32_t entry_opd, uint32_t stack_top);

/* This project's own: load whatever real system PRX modules the title needs
 * into guest RAM and register their exports. stubs.cpp has the empty version
 * a game with no lifted PRX wants. */
void     ps3_load_prx_modules(void);

extern const char* ppu_vfs_root;              /* host dir the PS3 mount points map into */

/* The status the guest handed to sys_process_exit. A title that exits that way
 * never returns through ppu_run, so this is only read on the path where the
 * entry function unwound instead. */
extern int     g_sys_process_exit_called;
extern int32_t g_sys_process_exit_code;

/* Host-provided symbols the runtime and the HLE libraries link against. */
uint8_t* vm_base = nullptr;
/* Second, never-protected view of the guest space (see alloc_guest_vm). */
uint8_t* vm_sudo_base = nullptr;
/* RSX page-protection fault handler: guest address, 1 = write. Returns 1 if handled. */
int (*g_vm_access_violation_hook)(uint32_t guest_addr, int is_write) = nullptr;
extern "C" void spu_jobtime_av(long long qpc_ticks);   /* spu_drain.c */
extern "C" int spu_jobtime_cur_img(void);              /* spu_drain.c: [av-who] */
extern "C" uint32_t g_sjt_link_b;                      /* spu_drain.c: WWS job-call return pc ([spu-jobs]) */
extern "C" int spu_sched_effective(void);              /* spu_channels.c: PORT_SPU_SCHED, [native] line */
/* Unclaimed protection fault on a guarded page: the RSX page-guard registry
 * re-applies (or drops) the page's protection itself (rsx_page_guard_reconcile;
 * set by the engine). Returns 1 when the access can be retried. */
int (*g_vm_guard_reconcile_hook)(uint32_t guest_addr, int is_write) = nullptr;
extern uint32_t ppu_vm_size;                  /* ppu_loader.cpp: the OOB guard */
extern uint32_t ppu_hle_inject_base;          /* cellGcmSys.c (v163 boot check) */

/* Guest-callback dispatch. g_ps3_guest_caller is the hook the HLE runtime
 * calls back into recompiled code through -- cellSysutil events and the GCM
 * vblank/flip handlers. ppu_guest_call does the OPD -> dispatch. */
typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
extern ps3_guest_caller_fn g_ps3_guest_caller;
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                        uint64_t, uint64_t, uint64_t, uint64_t);

/* The GCM half of the frame clock. */
void     cellGcmTickVBlank(void);
void     cellGcmTickFlip(void);
int      cellGcm_flip_pace_poll(long long now_us, long long period_us);
int      cellGcm_vblank_thread_start(long long vblank_us, long long frame_limit_us);
int      cellGcm_vblank_thread_running(void);
double   r2_fps_target(void);                 /* stubs.cpp: 0 = uncapped */
void     r2_patches_apply(void);              /* r2_patches.cpp */
double   r2_display_refresh_hz(void);
int      cellGcm_take_flip_pending(void);
void     cellGcm_rsx_process_fifo(void);      /* drain get -> put */
unsigned cellGcm_flip_request_count(void);
extern "C" volatile int g_r2pc_paused;   /* the port's pause (PC menu, quit prompt, pad loss): osd_repost */
}

/* CPU detection, PORT_CPU_PROFILE and thread placement (runs first in main). */
#include "cpu_profile.inc"
extern "C" int ps3_host_core_scarce(void);   /* sys_ppu_thread.c, PORT_CORE_SCARCE */
#include "port_cpucheck.h"  /* AVX2 (x86-64-v3) build: clear error on older CPUs */
#include "r2_crash.inc"
#include "port_native_summary.h"
#include "port_mem.h"         /* PORT_LARGE_PAGES: reported unavailable here, see main() */
extern "C" uint32_t r2nj_effective_mask(void);   /* r2_native_jobs.c */
extern "C" int r2nj_kernel_active(int k);
#include "r2_autoconfig.h"

/* ---------------------------------------------------------------------------
 * RSX present backend
 * -----------------------------------------------------------------------*/
/* D3D12 on Windows, Metal on Apple, and the null backend's headless software
 * path anywhere else -- the same selection runtime/host/host_posix.c and the
 * toolkit's boot harness make. All three expose the same three entry points,
 * so the frame clock below is backend-agnostic. */
#if defined(_WIN32)
extern "C" int  rsx_d3d12_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_d3d12_backend_present(void);
extern "C" int  rsx_d3d12_backend_pump_messages(void);
#  define rsx_backend_init    rsx_d3d12_backend_init
#  define rsx_backend_present rsx_d3d12_backend_present
#  define rsx_backend_pump    rsx_d3d12_backend_pump_messages
#  define RSX_BACKEND_NAME    "D3D12"
#elif defined(__APPLE__)
extern "C" int  rsx_metal_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_metal_backend_present(void);
extern "C" int  rsx_metal_backend_pump_messages(void);
#  define rsx_backend_init    rsx_metal_backend_init
#  define rsx_backend_present rsx_metal_backend_present
#  define rsx_backend_pump    rsx_metal_backend_pump_messages
#  define RSX_BACKEND_NAME    "Metal"
#else
extern "C" int  rsx_null_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_null_backend_present(void);
extern "C" int  rsx_null_backend_pump_messages(void);
#  define rsx_backend_init    rsx_null_backend_init
#  define rsx_backend_present rsx_null_backend_present
#  define rsx_backend_pump    rsx_null_backend_pump_messages
#  define RSX_BACKEND_NAME    "null (headless software)"
#endif

/* ---------------------------------------------------------------------------
 * Configuration
 * -----------------------------------------------------------------------*/
/* The flat VM must span every region the PS3 memory map uses, not just the
 * game image: sys_ppu_thread_create puts thread stacks at 0xD0000000, so a
 * smaller arena makes every spawned thread's stack access out of bounds. */
#define VM_SIZE       0x100010000ull   /* the full 32-bit guest space + a 64K guard */
#define STACK_TOP     0x0FF00000u      /* main-thread stack, below the 0x10000000 segment */
#define WINDOW_WIDTH  1280
#define WINDOW_HEIGHT 720

/* ---------------------------------------------------------------------------
 * Frame clock
 * -----------------------------------------------------------------------*/
/* On real hardware the RSX raises a vblank interrupt about 60 times a second
 * and that is what drives the game's frame loop. With no RSX it is synthesized
 * here: a host thread calls cellGcmTickVBlank/TickFlip, which invoke the
 * handlers the guest registered, and drains the GCM FIFO. Without this the
 * title initialises, registers its handlers and then waits forever.
 *
 * The ticks are driven off real elapsed time rather than off how long present()
 * took. A hidden or occluded window makes present block hard, and pacing the
 * ticks behind it paces the whole game behind it. */
static volatile LONG g_frames_presented = 0;

/* Frames handed to the backend at a guest FLIP boundary -- one per frame the
 * guest actually finished. The presents made before the guest's first flip, so
 * a fresh window is not left blank through a long boot, carry no guest frame
 * and are deliberately not counted. */
extern "C" unsigned ppu_boot_frames_presented(void)
{
    return (unsigned)g_frames_presented;
}

#if defined(_WIN32)
/* RSX_LIVE_DRAW=1: the register-file NV4097 -> D3D12 engine
 * (libs/video/rsx_live_draw.c) renders instead of rsx_d3d12_backend. The null
 * backend opens the window and the engine binds its swap chain to it, as the
 * toolkit's boot harness does. Unset, nothing below changes. */
extern "C" int   rsx_live_draw_enabled(void);
extern "C" int   rsx_live_draw_init(void* hwnd, uint32_t w, uint32_t h,
                                    const uint8_t* (*guest_ptr)(void*, uint32_t,
                                                                uint32_t, uint32_t),
                                    void* user);
extern "C" void  rsx_live_draw_present(uint32_t buffer_id);
extern "C" int   rsx_null_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" int   rsx_null_backend_pump_messages(void);
extern "C" void* rsx_null_backend_get_hwnd(void);
extern "C" void  rsx_null_backend_suppress_present(int on);
extern "C" uint32_t cellGcmResolveLocated(int local, uint32_t offset);
extern "C" uint32_t cellGcmResolveIO(uint32_t offset);
extern "C" unsigned cellGcmGetCurrentDisplayBufferId(void);
#include "../ps3recomp/libs/video/rpcs3_bridge/r2rsx_bridge.h"

static int s_rsx_live = 0;
static int s_rsx_vk = 0;

#if defined(PS3RECOMP_RSX_VULKAN)
#include "../ps3recomp/libs/video/rsx_vulkan_backend.h"
#include "../ps3recomp/libs/video/rsx_featcov.h"   /* rsx_featcov_dump at exit */
extern "C" int r2_native_aa_register(void);   /* r2_native_aa.cpp: RSX_FSR=2 DLAA, 3 FSR 3.1, 1 FSR1 */
/* R2_RSX=vulkan (or native): ps3recomp's register-file draw engine driving
 * the Vulkan backend, presenting into the null backend's window. */
static int rsx_vk_wanted(void)
{
    const char* e = getenv("R2_RSX");
    return e && (!_stricmp(e, "vulkan") || !_stricmp(e, "native"));
}
#endif

#include "../ps3recomp/libs/video/rsx_d3d12_engine.h"
extern "C" int r2_native_aa_register_d3d12(void);   /* r2_native_aa.cpp: RSX_FSR 1 FSR1, 2 DLAA, 3 FSR 3.1, 4 XeSS */
/* R2_RSX=d3d12: "Direct3D 12 (experimental)" -- the same register-file draw engine
 * driving the D3D12 backend (rsx_d3d12_engine.c), presenting into the null
 * backend's window. Offered on its own, not behind the native-Vulkan hide. */ int s_rsx_d3e = 0; int rsx_d3e_wanted(void)
{
    const char* e = getenv("R2_RSX");
    return e && !_stricmp(e, "d3d12");
}

/* The native Vulkan renderer is hidden from the launcher for now (still built:
 * the draw engine is shared). An old settings.bat with R2_RSX=vulkan/native
 * runs the RPCS3 renderer instead (once per start, logged), and the native
 * renderer's RSX_FSR=3 (FSR 3.1) becomes the RPCS3 renderer's 1. Dev
 * override: R2_RENDERER_FORCE_NATIVE=1 keeps it. Runs before anything reads
 * R2_RSX (rsx_vk_wanted, r2rsx_enabled, r2_pcport). */
static void r2_renderer_migrate(void)
{
    const char* e = getenv("R2_RSX");
    /* Without a usable D3D12 device the Direct3D 12 (experimental) choice falls back to the RPCS3 renderer
     * here, before anything reads R2_RSX. */
    if (e && !_stricmp(e, "d3d12")) {
        if (!rsx_d3d12_engine_probe()) {
            fprintf(stderr, "[renderer] R2_RSX=d3d12: Direct3D 12 failed its start-up probe (device, shaders, swap chain); using the RPCS3 renderer\n");
            rsx_d3d12_env_to_rpcs3();   /* also renumbers RSX_FSR */
        }
        return;
    }
    if (!e || (_stricmp(e, "vulkan") && _stricmp(e, "native"))) return;
    const char* f = getenv("R2_RENDERER_FORCE_NATIVE");
    if (f && *f && atoi(f) != 0) {
        fprintf(stderr, "[renderer] R2_RSX=%s kept (R2_RENDERER_FORCE_NATIVE=%s): native Vulkan renderer, experimental\n", e, f);
        return;
    }
    fprintf(stderr, "[renderer] R2_RSX=%s: the native Vulkan renderer is no longer offered, using the RPCS3 renderer "
                    "(R2_RENDERER_FORCE_NATIVE=1 keeps it; open the launcher once to update settings.bat)\n", e);
    _putenv_s("R2_RSX", "rpcs3");
    if (const char* fsr = getenv("RSX_FSR"); fsr && !strcmp(fsr, "3")) _putenv_s("RSX_FSR", "1");
}

/* location 0 = RSX local memory, 1 = main memory through the IO table. */
static const uint8_t* rsx_live_guest_ptr(void*, uint32_t location,
                                         uint32_t offset, uint32_t min_bytes)
{
    if (!vm_base) return nullptr;
    uint32_t ea;
    if (location == 0) ea = cellGcmResolveLocated(1, offset);
    else {
        ea = cellGcmResolveIO(offset);
        if (!ea) ea = cellGcmResolveLocated(0, offset);
    }
    if (!ea || ea == 0xFFFFFFFFu || (uint64_t)ea + min_bytes > 0x100000000ull) return nullptr;
    return (const uint8_t*)vm_base + ea;
}

static int rsx_select_init(uint32_t w, uint32_t h, const char* title)
{
#if defined(PS3RECOMP_RSX_VULKAN)
    if (rsx_vk_wanted()) {
        _putenv("PS3RECOMP_VK_GUEST_PROGRAMS=1");
        if (!getenv("PS3RECOMP_RSX_ENGINE")) _putenv("PS3RECOMP_RSX_ENGINE=dispatch");
        if (rsx_null_backend_init(w, h, title) == 0) {
            rsx_vulkan_backend_set_window(rsx_null_backend_get_hwnd());
            r2_native_aa_register();
            if (rsx_vulkan_backend_init(w, h, title) == 0) {
                s_rsx_vk = 1;
                rsx_null_backend_suppress_present(1);
                fprintf(stderr, "[rsx] native Vulkan renderer up\n");
                return 0;
            }
        }
        fprintf(stderr, "[rsx] native Vulkan init FAILED -- falling back to the D3D12 backend\n");
    }
#endif
    if (rsx_d3e_wanted()) {
        if (!getenv("PS3RECOMP_RSX_ENGINE")) _putenv("PS3RECOMP_RSX_ENGINE=dispatch");
        if (rsx_null_backend_init(w, h, title) == 0) {
            rsx_d3d12_engine_set_window(rsx_null_backend_get_hwnd());
            r2_native_aa_register_d3d12();
            if (rsx_d3d12_engine_init(w, h, title) == 0) {
                s_rsx_d3e = 1;
                rsx_null_backend_suppress_present(1);
                fprintf(stderr, "[rsx] Direct3D 12 (experimental) renderer up\n");
                return 0;
            }
        }
        /* main() probed the device already (r2_renderer_migrate), so this is rare. The RPCS3 renderer
         * can't be started this late (its FIFO owner is chosen at the first GCM call), so the game
         * restarts itself with it; never the legacy backend. */
        fprintf(stderr, "[rsx] Direct3D 12 (experimental) init FAILED -- restarting with the Compatible (RPCS3) renderer\n");
        fflush(stderr);
        if (rsx_d3d12_engine_relaunch_compatible()) ExitProcess(0);
        MessageBoxA(nullptr, "Direct3D 12 (experimental) could not start on this system.\n\n"
                             "Choose the Compatible renderer in the launcher.", "Renderer error", MB_ICONERROR);
        ExitProcess(1);
    }
    if (rsx_live_draw_enabled()) {
        if (rsx_null_backend_init(w, h, title) == 0 &&
            rsx_live_draw_init(rsx_null_backend_get_hwnd(), w, h, rsx_live_guest_ptr, nullptr) == 0) {
            s_rsx_live = 1;
            rsx_null_backend_suppress_present(1);
            fprintf(stderr, "[rsx] live-draw engine up (D3D12)\n");
            return 0;
        }
        fprintf(stderr, "[rsx] live-draw init FAILED -- falling back to the D3D12 backend\n");
    }
    return rsx_backend_init(w, h, title);
}
static void rsx_select_present(void)
{
#if defined(PS3RECOMP_RSX_VULKAN)
    if (s_rsx_vk) { rsx_vulkan_backend_present(); return; }
#endif
    if (s_rsx_d3e) { rsx_d3d12_engine_present(); return; }
    if (s_rsx_live) rsx_live_draw_present(cellGcmGetCurrentDisplayBufferId());
    else rsx_backend_present();
}
static int rsx_select_pump(void)
{
    return (s_rsx_live || s_rsx_vk || s_rsx_d3e) ? rsx_null_backend_pump_messages() : rsx_backend_pump();
}
#  undef  rsx_backend_init
#  undef  rsx_backend_present
#  undef  rsx_backend_pump
#  define rsx_backend_init    rsx_select_init
#  define rsx_backend_present rsx_select_present
#  define rsx_backend_pump    rsx_select_pump
#endif

static void present_guest_frame(void)
{
    rsx_backend_present();
    /* This thread increments and guest threads read, so interlocked rather
     * than a volatile ++, which on arm64 is neither atomic nor a fence. */
    InterlockedIncrement(&g_frames_presented);
}

extern "C" uint32_t cellGcm_rsx_put_snapshot(void);
extern "C" int      cellGcm_rsx_work_pending(void);

/* Resistance port (v86): the vblank clock in microseconds from QPC.
 * GetTickCount64 only moves in ~15.6 ms steps (timeBeginPeriod does not
 * change it), so the 60 Hz beat that completes flips fired on a jittery
 * 0/15.6/31.2 ms grid and a finished frame often waited a whole extra beat
 * for its flip. RSX_VBLANK_US overrides the period (default 16667 = 60 Hz). */
static long long fc_us(void)
{
    static LARGE_INTEGER f = { 0 };
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (long long)((double)c.QuadPart * 1e6 / (double)f.QuadPart);
}

extern "C" void r2pf_toast(const char* title, const char* text, char badge);   /* r2_pcfeat.cpp */
static DWORD WINAPI frame_clock(LPVOID)
{
    port_place_walker();
    const char* title = getenv("PS3_TITLE");
    if (!title || !*title) title = "ps3recomp";

    /* R2_RSX=rpcs3: RPCS3's RSX core renders (Vulkan) and runs its own FIFO,
     * vblank and flip timing. This thread only owns the window. */
    if (r2rsx_enabled()) {
        if (rsx_null_backend_init(WINDOW_WIDTH, WINDOW_HEIGHT, title) != 0) {
            fprintf(stderr, "[rsx] RPCS3 mode: window creation FAILED\n");
            return 0;
        }
        rsx_null_backend_suppress_present(1);
        r2rsx_set_window(rsx_null_backend_get_hwnd(), WINDOW_WIDTH, WINDOW_HEIGHT);
        fprintf(stderr, "[rsx] RPCS3 RSX core (Vulkan) -- window open\n");
        while (rsx_null_backend_pump_messages() == 0) Sleep(4);
        rsx_featcov_dump("exit");
        ExitProcess(0);
    }

    int rsx_ok = (rsx_backend_init(WINDOW_WIDTH, WINDOW_HEIGHT, title) == 0);
    fprintf(stderr, "[rsx] %s backend init %s\n", RSX_BACKEND_NAME,
            rsx_ok ? "OK -- window open" : "FAILED");
    {   /* frame generation only exists in the RPCS3 renderer: tell the user instead of silently ignoring it */
        const char* fg = getenv("R2_DLSS_FG");
        if (fg && *fg && strcmp(fg, "0") != 0) {
            fprintf(stderr, "[fg] frame generation off: R2_DLSS_FG=%s needs the rpcs3 renderer (R2_RSX=rpcs3, launcher Renderer \"RPCS3\")\n", fg);
            r2pf_toast("Frame generation is off", "It needs the RPCS3 renderer (launcher: Renderer).", '!');
        }
    }

    unsigned  last_flip = 0;
    long long next_tick = fc_us();
    static long long vb_us = -1;
    /* RPCS3 vblank thread: period = (1'000'000 + vblank_ntsc * 1000) / vblank_rate us;
     * rpcs3_config.yml has Vblank Rate 60, NTSC fixup off -> 60 Hz (16667 us),
     * not 59.94. RSX_CFG_LEGACY=1 restores 16683. */
    if (vb_us < 0) {
        const char* l = getenv("RSX_CFG_LEGACY");
        const long long def = (l && *l && *l != '0') ? 16683 : 16667;
        const char* e = getenv("RSX_VBLANK_US"); vb_us = e ? atoll(e) : def; if (vb_us < 1000) vb_us = def;
        /* R2 paces its frames on vblank: at 60 Hz it never exceeds 60 fps
         * whatever R2_FPS asks (measured: R2_FPS=120 -> 60.0). Like RPCS3's
         * Vblank Rate setting used with unlocked-fps patches, the vblank rate
         * follows the target above 60; uncapped uses max(display, 240) Hz.
         * RSX_VBLANK_US overrides; GCM_VBLANK_FOLLOW_FPS=0 keeps 60 Hz. */
        const char* f = getenv("GCM_VBLANK_FOLLOW_FPS");
        if (!e && !(f && *f == '0')) {
            const double fps = r2_fps_target();
            double hz = fps;
            if (fps <= 0.0) { hz = r2_display_refresh_hz(); if (hz < 240.0) hz = 240.0; }
            if (hz > 60.5) { vb_us = (long long)(1e6 / hz + 0.5); if (vb_us < 1000) vb_us = 1000; }
        }
    }

    /* RPCS3 handle_emu_flip's frame limit: how soon after the previous flip a
     * flip may complete. GCM_FRAME_LIMIT=auto (default): one vblank (RPCS3
     * "Auto") up to 60 fps, or a little sooner when R2_FPS asks for a faster
     * display; off above 60 / uncapped, where the game limiter
     * (r2_frame_period) is the only pacer (RPCS3's Unlock FPS patch wants
     * frame limit Off). GCM_FRAME_LIMIT=off|vblank|<Hz> overrides. */
    long long limit_us;
    {
        const double fps = r2_fps_target();
        const char* e = getenv("GCM_FRAME_LIMIT");
        if (e && (!_stricmp(e, "off") || !_stricmp(e, "0") || !_stricmp(e, "none"))) limit_us = 0;
        else if (e && !_stricmp(e, "vblank")) limit_us = vb_us;
        else if (e && atof(e) >= 1.0) limit_us = (long long)(1e6 / atof(e));
        else if (fps > 0.0 && fps <= 60.5) {
            const long long f = (long long)(1e6 / fps);
            limit_us = f < vb_us ? f : vb_us;
        } else limit_us = 0;
    }
    /* RPCS3 vblank thread: vblank, flip completion and handler delivery on a
     * thread of their own. GCM_VBLANK_THREAD=0 keeps them in this loop. */
    const int vthread = cellGcm_vblank_thread_start(vb_us, limit_us);
    fprintf(stderr, "[rsx] vblank %lld us, frame limit %lld us, %s\n", vb_us, limit_us,
            vthread ? "vblank thread" : "host-loop ticks");

    for (;;) {
        /* Resistance port: wake as soon as the guest publishes new FIFO work
         * (its `put` moves) instead of a blind Sleep(4). The guest fences many
         * passes per frame on labels this drain writes, so each blind sleep was
         * paid several times a frame. Spin-yield up to ~2 ms, then sleep 1 ms.
         * RSX_SPINWAIT=0 restores Sleep(4). */
        {
            static int spin = -1;
            if (spin < 0) { const char* e = getenv("RSX_SPINWAIT"); spin = e ? atoi(e) : 1; }
            if (!spin) Sleep(4);
            else {
                static uint32_t last_put = 0;
                uint32_t p0 = cellGcm_rsx_put_snapshot();
                if (p0 == last_put) {
                    /* Resistance port: keep spinning until the next vblank
                     * tick is due instead of falling into Sleep(1) after 2 ms.
                     * The guest ping-pongs with this thread many times a frame
                     * (kick a segment, wait for its label); every Sleep(1) was
                     * 1-2 ms of dead latency per round trip. Costs one core.
                     * RSX_SPINWAIT=2 restores the 2 ms + Sleep(1) behaviour. */
                    ULONGLONG t0 = GetTickCount64();
                    if (spin == 2) {
                        while (cellGcm_rsx_put_snapshot() == p0 && GetTickCount64() - t0 < 2)
                            SwitchToThread();
                        if (cellGcm_rsx_put_snapshot() == p0) Sleep(1);
                    } else if (spin == 3) {
                        unsigned k = 0;
                        while (cellGcm_rsx_put_snapshot() == p0 &&
                               fc_us() - next_tick < 0) {
                            if (++k & 63u) YieldProcessor();
                            else {
                                if (cellGcm_flip_pace_poll(fc_us(), vb_us)) break;
                                /* deferred labels wait on GPU queries: walk */
                                if (cellGcm_rsx_work_pending()) break;
                                SwitchToThread();
                            }
                        }
                    } else {
                        /* Spin briefly (the guest's kick/wait-label round
                         * trips are microseconds apart), then poll on a
                         * high-resolution timer. Spinning with SwitchToThread
                         * until the next vblank burned ~half a core and ~180k
                         * context switches/s idle in the menu. The guest's put
                         * is a plain store, so there is nothing to wake on.
                         * RSX_SPINWAIT=3 restores the full spin; RSX_IDLE_US /
                         * RSX_SPIN_US tune the poll period / spin window. */
                        static long long spin_us = -1, idle_us = -1;
                        static HANDLE tmr = NULL;
                        if (spin_us < 0) {
                            const char* e = getenv("RSX_SPIN_US");  spin_us = e ? atoll(e) : 150;
                            e = getenv("RSX_IDLE_US"); idle_us = e ? atoll(e) : 100;
                            if (idle_us < 10) idle_us = 10;
                            tmr = CreateWaitableTimerExW(NULL, NULL,
                                      0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
                                      TIMER_ALL_ACCESS);
                            if (!tmr) { timeBeginPeriod(1); tmr = CreateWaitableTimerW(NULL, TRUE, NULL); }
                        }
                        const long long ts = fc_us();
                        unsigned k = 0;
                        for (;;) {
                            if (cellGcm_rsx_put_snapshot() != p0) break;
                            const long long now_s = fc_us();
                            if (now_s - next_tick >= 0) break;
                            if (now_s - ts < spin_us) {
                                if (++k & 63u) { YieldProcessor(); continue; }
                            }
                            if (cellGcm_flip_pace_poll(now_s, vb_us)) break;
                            if (cellGcm_rsx_work_pending()) break;
                            if (now_s - ts < spin_us) continue;
                            long long w = next_tick - now_s;
                            if (w > idle_us) w = idle_us;
                            if (tmr) {
                                LARGE_INTEGER due; due.QuadPart = -(w * 10);
                                if (SetWaitableTimer(tmr, &due, 0, NULL, NULL, FALSE))
                                    WaitForSingleObject(tmr, 2);
                                else Sleep(0);
                            } else Sleep(0);
                        }
                    }
                }
                last_put = cellGcm_rsx_put_snapshot();
            }
        }
        long long now = fc_us();

        int fired = 0;
        /* With the vblank thread the beat here only paces the drain and
         * present: one per period, never a burst after a stall. */
        while (now - next_tick >= 0 && fired < (vthread ? 1 : 240)) {
            cellGcmTickVBlank();
            cellGcmTickFlip();
            /* Present a pending flip BEFORE draining any further. The flip
             * fires at a get==put frame boundary on the guest thread, so the
             * batch held right now is exactly the completed frame; presenting
             * after the drain races the guest's next-frame writes and shows a
             * mixed one. */
            if (rsx_ok && cellGcm_take_flip_pending()) {
                present_guest_frame();
                last_flip = cellGcm_flip_request_count();
            }
            /* Drain the FIFO every tick. This is what writes the RSX sync-fence
             * labels the game's per-frame logic blocks on, so it has to keep
             * advancing at 60 Hz even while present() throttles. */
            if (rsx_ok) cellGcm_rsx_process_fifo();
            next_tick += vb_us;          /* ~60 Hz */
            fired++;
        }
        if (fired >= 240) next_tick = now;   /* fell too far behind -- resync */
        if (vthread && now - next_tick >= 0) next_tick = now + vb_us;

        /* Drain at the outer cadence too, not only on the 16 ms tick: titles
         * fence every render pass on a label the drain writes, and one wave of
         * those at 16 ms apiece paces the guest into single-figure frame rates.
         * The real RSX writes them in microseconds. */
        cellGcm_flip_pace_poll(fc_us(), vb_us);
        if (rsx_ok) {
            if (cellGcm_take_flip_pending()) {
                present_guest_frame();
                last_flip = cellGcm_flip_request_count();
            }
            cellGcm_rsx_process_fifo();
            /* A flip the drain just reached: present now, not a loop later. */
            if (cellGcm_take_flip_pending()) {
                present_guest_frame();
                last_flip = cellGcm_flip_request_count();
            }

            if (rsx_backend_pump() != 0) {
                /* window closed (Alt+F4 / X): end the game as the RPCS3 renderer does
                 * (frame_clock above); only clearing rsx_ok left it running headless */
                fprintf(stderr, "[rsx] window closed -- exiting\n");
                rsx_featcov_dump("exit");
                fflush(stdout); fflush(stderr);
                ExitProcess(0);
            }
#if defined(_WIN32)
            /* D3D12: the port UI changed but no flip came for ~33 ms (the PC menu, quit prompt or
             * pad-loss pause park the game): present the last frame again with the new UI, as
             * r2rsx_osd kicks the RPCS3 renderer. */
            if (s_rsx_d3e) rsx_d3d12_engine_osd_repost(33, g_r2pc_paused != 0 || cellGcm_flip_request_count() == 0);
#endif
            /* Present only a flip the drain has reached (s_flip_pending, taken
             * above). Presenting on a change of the flip-request counter could
             * show a buffer before the walker executed its flip, and presented
             * the same frame a second time. Before the first flip present
             * freely, so the window is not blank during boot. */
            if (cellGcm_flip_request_count() == 0)
                rsx_backend_present();
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * VFS root
 * -----------------------------------------------------------------------*/
/* Derive the directory holding PS3_GAME from the EBOOT path
 * <root>/PS3_GAME/USRDIR/EBOOT.elf -> <root>. $PS3_VFS_ROOT overrides. */
static char s_vfs_root[1024];

static void derive_vfs_root(const char* eboot)
{
    const char* env = getenv("PS3_VFS_ROOT");
    if (env && *env) { ppu_vfs_root = env; return; }

    strncpy(s_vfs_root, eboot, sizeof s_vfs_root - 1);
    for (char* p = s_vfs_root; *p; p++) if (*p == '\\') *p = '/';
    /* strip EBOOT.elf, USRDIR and PS3_GAME */
    for (int i = 0; i < 3; i++) { char* s = strrchr(s_vfs_root, '/'); if (s) *s = 0; }
    if (!s_vfs_root[0]) strcpy(s_vfs_root, ".");
    ppu_vfs_root = s_vfs_root;
}

/* ---------------------------------------------------------------------------
 * Guest VM
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
/* Demand-paging for the flat VM: reserve the whole 4 GB guest space, which
 * costs no commit, and commit each 64 KB page on first access. Every 32-bit
 * guest offset is then valid, so a garbage guest pointer reads zero instead of
 * killing the process. Faults outside the arena fall through untouched. */
/* The guest space is a pagefile-backed section mapped twice: vm_base, which the
 * title and the runtime use, and vm_sudo_base, an always-writable alias. The
 * RPCS3 RSX core write-protects guest pages that hold cached textures and render
 * targets in the vm_base view only and reads/writes through the sudo view.
 * A fault on a committed vm_base page is such a protection fault and goes to
 * g_vm_access_violation_hook; only reserved pages are demand-committed, one
 * 4 KB page at a time, so committing never resets a neighbour's protection. */
/* Pages seen committed, one bit per 4 KB page per view. Guest pages are never
 * decommitted, so a set bit lets the protection-fault path skip VirtualQuery. */
static volatile LONG vm_committed_bits[2][(VM_SIZE / 0x1000 + 31) / 32];
static inline bool vm_known_committed(int view, uintptr_t off)
{
    const size_t pg = off >> 12;
    return (vm_committed_bits[view][pg >> 5] >> (pg & 31)) & 1;
}
static inline void vm_mark_committed(int view, uintptr_t off)
{
    const size_t pg = off >> 12;
    _InterlockedOr(&vm_committed_bits[view][pg >> 5], (LONG)(1u << (pg & 31)));
}
static void vm_commit_range(uint8_t* p, size_t len)
{
    for (uint8_t* q = p; q < p + len; q += 0x1000) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(q, &mbi, sizeof(mbi)) && mbi.State == MEM_RESERVE)
            VirtualAlloc(q, 0x1000, MEM_COMMIT, PAGE_READWRITE);
    }
}
/* R2_AV_WHO (default 1): who reads/writes RSX-protected guest memory when the fault
 * hook takes >= 1 ms (a render-target readback on the faulting thread). The first
 * 64 distinct (64 KB region, host PC) pairs are logged once as
 *   [av-who] guest 0x.. read 4.1 ms thread 'name' spu-job 422 | func_A+0x.. < func_B+0x.. ...
 * with the lifted PPU functions found on the host stack (nearest entry <= frame PC). */
static bool av_who_on(void)
{
    static int on = -1;
    if (on < 0) { const char* v = getenv("R2_AV_WHO"); on = !(v && v[0] == '0'); }
    return on != 0;
}
static int av_who_guest(uintptr_t pc, uint32_t* off)
{
    uintptr_t best_h = 0; uint32_t best_g = 0;
    for (uint64_t k = 0; k < function_table_count; k++) {
        const uintptr_t h = (uintptr_t)function_table[k].func;
        if (h <= pc && h > best_h) { best_h = h; best_g = function_table[k].addr; }
    }
    if (!best_g || pc - best_h >= 0x20000) return 0;
    *off = (uint32_t)(pc - best_h);
    return (int)best_g;
}
static void av_who_note(const CONTEXT* ctx_in, uint32_t guest, int is_write, double ms)
{
    static volatile LONG s_n = 0;
    static uint64_t s_keys[64];
    if (s_n >= 64) return;
    const uint64_t key = ((uint64_t)(guest >> 16) << 48) ^ (uint64_t)ctx_in->Rip;
    static SRWLOCK s_lock = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&s_lock);
    bool dup = false;
    for (LONG i = 0; i < s_n; i++) dup |= s_keys[i] == key;
    if (dup || s_n >= 64) { ReleaseSRWLockExclusive(&s_lock); return; }
    s_keys[s_n] = key; InterlockedIncrement(&s_n);
    ReleaseSRWLockExclusive(&s_lock);

    char chain[512]; size_t len = 0; chain[0] = 0;
    CONTEXT c = *ctx_in;
    int found = 0;
    for (int d = 0; d < 32 && c.Rip && found < 6 && len < sizeof chain - 48; d++) {
        uint32_t off = 0;
        const uint32_t g = (uint32_t)av_who_guest((uintptr_t)c.Rip, &off);
        if (g) len += snprintf(chain + len, sizeof chain - len, "%sfunc_%08X+0x%X", found++ ? " < " : "", g, off);
        DWORD64 base = 0;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &base, NULL);
        if (!f) { c.Rip = *(DWORD64*)c.Rsp; c.Rsp += 8; continue; }
        PVOID hd; DWORD64 ef;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, f, &c, &hd, &ef, NULL);
    }
    char name[64] = "?";
    typedef HRESULT (WINAPI *gtd_t)(HANDLE, PWSTR*);
    static gtd_t gtd = (gtd_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
    PWSTR w = NULL;
    if (gtd && SUCCEEDED(gtd(GetCurrentThread(), &w)) && w) {
        if (w[0]) snprintf(name, sizeof name, "%ls", w);
        LocalFree(w);
    }
    DWORD64 mb = 0;
    RtlLookupFunctionEntry(ctx_in->Rip, &mb, NULL);
    fprintf(stderr, "[av-who] guest 0x%08X %s %.2f ms thread '%s' (tid %lu) spu-job %d pc %s+0x%llX | %s\n",
            guest, is_write ? "write" : "read", ms, name, GetCurrentThreadId(), spu_jobtime_cur_img(),
            mb ? "img" : "?", (unsigned long long)(ctx_in->Rip - mb), found ? chain : "no lifted PPU frame");
    fflush(stderr);
}
static LONG WINAPI vm_commit_veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        const ULONG_PTR fault = ep->ExceptionRecord->ExceptionInformation[1];
        const int is_write = ep->ExceptionRecord->ExceptionInformation[0] == 1;
        uint8_t* const bases[2] = { vm_base, vm_sudo_base };
        for (int i = 0; i < 2; i++) {
            const uintptr_t base = (uintptr_t)bases[i];
            if (!base || fault < base || fault >= base + VM_SIZE) continue;
            MEMORY_BASIC_INFORMATION mbi = {};
            if (!vm_known_committed(i, fault - base)) {
                if (VirtualQuery((void*)fault, &mbi, sizeof(mbi)) && mbi.State == MEM_RESERVE) {
                    vm_commit_range((uint8_t*)(fault & ~(uintptr_t)0xFFFF), 0x10000);
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
                if (mbi.State == MEM_COMMIT)
                    vm_mark_committed(i, fault - base);
            }
            if (i == 0 && g_vm_access_violation_hook) {
                /* [spu-jobs]: fault-hook time inside a timed SPU job */
                LARGE_INTEGER avq0, avq1; QueryPerformanceCounter(&avq0);
                const int claimed = g_vm_access_violation_hook((uint32_t)(fault - base), is_write);
                QueryPerformanceCounter(&avq1);
                spu_jobtime_av(avq1.QuadPart - avq0.QuadPart);
                if (claimed && av_who_on()) {
                    static LARGE_INTEGER s_qf;
                    if (!s_qf.QuadPart) QueryPerformanceFrequency(&s_qf);
                    const double ms = (double)(avq1.QuadPart - avq0.QuadPart) * 1000.0 / (double)s_qf.QuadPart;
                    if (ms >= 1.0) av_who_note(ep->ContextRecord, (uint32_t)(fault - base), is_write, ms);
                }
                if (claimed) return EXCEPTION_CONTINUE_EXECUTION;
            }
            /* Another thread may have lifted the protection while this fault was
             * dispatched (RSX invalidation racing a SPU DMA): just retry. */
            if (VirtualQuery((void*)fault, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT &&
                (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE |
                                (is_write ? 0 : (PAGE_READONLY | PAGE_EXECUTE_READ)))))
                return EXCEPTION_CONTINUE_EXECUTION;
            /* Unclaimed protection fault on a committed guest page (e.g. RSX cache
             * state lost): open the page rather than crash the title. */
            if (i == 0 && g_vm_access_violation_hook && mbi.State == MEM_COMMIT &&
                !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) {
                static volatile LONG warned = 0;
                if (InterlockedExchange(&warned, 1) == 0)
                    fprintf(stderr, "[vm] unclaimed protection fault at guest 0x%08X; unprotecting\n",
                            (uint32_t)(fault - base));
                /* Through the guard registry, so its owner mask and the
                 * caches keyed on protection stay in step with the page. */
                if (g_vm_guard_reconcile_hook) {
                    if (g_vm_guard_reconcile_hook((uint32_t)(fault - base), is_write))
                        return EXCEPTION_CONTINUE_EXECUTION;
                }
                DWORD old;
                if (VirtualProtect((void*)(fault & ~(uintptr_t)0xFFF), 0x1000, PAGE_READWRITE, &old))
                    return EXCEPTION_CONTINUE_EXECUTION;
            }
            break;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

extern "C" void vm_inline_refresh(void);
static bool alloc_guest_vm(void)
{
#ifdef _WIN32
    AddVectoredExceptionHandler(1, vm_commit_veh);
    {
        HANDLE sec = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE | SEC_RESERVE,
                                        (DWORD)(VM_SIZE >> 32), (DWORD)VM_SIZE, NULL);
        if (sec) {
            vm_base      = (uint8_t*)MapViewOfFile(sec, FILE_MAP_ALL_ACCESS, 0, 0, (SIZE_T)VM_SIZE);
            vm_sudo_base = (uint8_t*)MapViewOfFile(sec, FILE_MAP_ALL_ACCESS, 0, 0, (SIZE_T)VM_SIZE);
            CloseHandle(sec);   /* the views keep the section alive */
        }
        if (!vm_base || !vm_sudo_base) {
            fprintf(stderr, "[vm] section mapping failed (%lu); falling back to a single reserve\n",
                    GetLastError());
            if (vm_base) UnmapViewOfFile(vm_base);
            if (vm_sudo_base) UnmapViewOfFile(vm_sudo_base);
            vm_sudo_base = nullptr;
            vm_base = (uint8_t*)VirtualAlloc(NULL, VM_SIZE, MEM_RESERVE, PAGE_READWRITE);
        }
    }
    ppu_vm_size = 0;              /* the whole space is backed; no OOB guard needed */
#else
    /* No vectored exception handlers off Windows, so the arena is mapped up
     * front and lazily backed by the OS: only pages the title touches cost
     * anything. It stops below the 0xE0000000 mark, so ppu_vm_size arms the
     * loader's bounds check for what is left.
     *
     * mmap, not calloc: vm_commit() hands vm_base + addr to mprotect, which
     * takes page-aligned addresses only. glibc serves an allocation this big
     * from its own mmap chunk and returns a pointer 16 bytes past the page
     * start, so every commit failed with EINVAL and sys_memory_allocate
     * answered ENOMEM to every call. mmap's base is page-aligned and zeroed. */
    void* p = mmap(NULL, 0xE0000000u, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    vm_base = (p == MAP_FAILED) ? nullptr : (uint8_t*)p;
    ppu_vm_size = 0xE0000000u;
#endif
    vm_inline_refresh();   /* ppu_recomp.h inline bounds (ppu_loader.cpp) */
    /* v163: the lifted game code is built with these bounds as constants
     * (PPU_FIXED_BOUNDS): VM end 0xE0000000, gcm ref-poll word 0x20002008. */
    { if (ppu_hle_inject_base != 0x20000000u || (ppu_vm_size && ppu_vm_size != 0xE0000000u))
          fprintf(stderr, "[vm] WARNING: inject base 0x%08X / vm size 0x%08X differ from the PPU_FIXED_BOUNDS constants\n",
                  ppu_hle_inject_base, ppu_vm_size); }
    return vm_base != nullptr;
}

/* ---------------------------------------------------------------------------
 * Main
 * -----------------------------------------------------------------------*/
static void harness_guest_caller(uint32_t opd, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4,
                                 uint64_t a5, uint64_t a6, uint64_t a7)
{
    ppu_guest_call(opd, a0, a1, a2, a3, a4, a5, a6, a7);
}

#ifdef _WIN32
#include <intrin.h>
#include "ps3emu/port_hitch.h"   /* g_port_hitch_job_tag: [spike] / [spu-jobs] job classes */
/* R2 perf 1115: SPU job image id -> class, from static analysis of the job images (strings in the
 * extracted jobmods / the EBOOT wwspic ranges; see files\r2_perf_1115.md). "phys" = igPhysicsColl
 * (rigid-body collision + simulate + post-update, jobmod 019fca988b49), "pcf" = the dynamicjoint
 * constraint framework (ragdoll / joint builders, pcf_solver_sim / pcf_solver_ik), "limb" = zombie limb
 * physics. water / geom / mesh from native_hot_r2.md. PORT_SPU_JOB_TAGS=0 = no tags. */
static const char* r2_spu_job_tag(int img)
{
    switch (img) {
    case 201: return "phys";
    case 605: return "limb";
    case 216: return "water";      /* 7db0: water normal / upblend */
    case 209: return "ocean";      /* 4154: ocean FFT phasor */
    case 227: return "geom";       /* dd413: geometry xform / cull */
    case 222: return "mesh";       /* b126: triangulate (packed mesh / edge lock data) */
    default: break;
    }
    if (img >= 649 && img <= 671) return "pcf";
    return nullptr;
}
/* [spike] cause: R2 frame-loop guest wait sites (port_hitch.h g_port_hitch_site_name), "class:name".
 * Sources: files\r2_spu_wait_1116.md, stubs.cpp R2_JOBWAIT_WAKE, r2_hot.cpp r2_occ_wait. */
static const char* r2_wait_site_name(uint32_t lr)
{
    switch (lr) {
    case 0x001723C8u: return "rsx:occlusion-report";   /* func_006483A8 via r2_occ_wait: last frame's reports */
    case 0x0026C1E0u: return "rsx:occlusion-report";   /* r2_occ_wait's second caller */
    case 0x006364A0u: return "rsx:frame-label";        /* func_00636268: RSX frame label poll */
    case 0x006B8D3Cu: return "spu:job-list";           /* func_006B8BB4: usleep(128) poll of SPU job-list progress */
    case 0x00B20AD4u: return "movie:bink";
    default: return nullptr;
    }
}
/* Resistance port: one [sys] line so a tester's run.log says what it ran on,
 * and a readable message instead of an illegal-instruction crash on CPUs
 * without the x86-64-v3 set (AVX2/FMA/BMI2) the lifted code is built for. */
static void port_sys_report(void)
{
    int r[4]; char brand[49] = {0};
    __cpuid(r, 0x80000000);
    if ((unsigned)r[0] >= 0x80000004u)
        for (int i = 0; i < 3; i++) { __cpuid(r, 0x80000002 + i); memcpy(brand + 16 * i, r, 16); }
    __cpuid(r, 1);
    const int fma = (r[2] >> 12) & 1, osx = (r[2] >> 27) & 1, avx = (r[2] >> 28) & 1;
    __cpuidex(r, 7, 0);
    const int avx2 = (r[1] >> 5) & 1, bmi2 = (r[1] >> 8) & 1;
    const int ymm = osx && ((_xgetbv(0) & 6) == 6);
    SYSTEM_INFO si; GetSystemInfo(&si);
    MEMORYSTATUSEX ms; ms.dwLength = sizeof ms; GlobalMemoryStatusEx(&ms);
    OSVERSIONINFOW ov = {0}; ov.dwOSVersionInfoSize = sizeof ov;
    typedef LONG (WINAPI *rtlgv_t)(OSVERSIONINFOW*);
    rtlgv_t rtlgv = (rtlgv_t)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
    if (rtlgv) rtlgv(&ov);
    const char* b = brand; while (*b == ' ') b++;
    printf("[sys] CPU: %s | %lu logical CPUs | RAM %.1f GB (%.1f GB free) | Windows %lu.%lu.%lu | AVX2 %d FMA %d BMI2 %d\n",
           *b ? b : "unknown", (unsigned long)si.dwNumberOfProcessors,
           ms.ullTotalPhys / 1073741824.0, ms.ullAvailPhys / 1073741824.0,
           ov.dwMajorVersion, ov.dwMinorVersion, ov.dwBuildNumber, avx && avx2 && ymm, fma, bmi2);
    if (!(avx && avx2 && fma && bmi2 && ymm)) {
        fprintf(stderr, "ERROR: this CPU lacks AVX2/FMA/BMI2, which this build requires\n");
        MessageBoxA(NULL, "This build needs a CPU with AVX2/FMA/BMI2 (Intel Haswell / AMD Zen or newer).\n"
                          "Your CPU does not support it, so the game cannot run.", "Unsupported CPU", MB_ICONERROR);
        exit(1);
    }
}
#endif

extern "C" void cellGame_init_from_paramsfo(const char* sfo_path);

int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage: %s <PPU ELF>\n", argv[0]);
        return 2;
    }
    /* First, as R3: the preset writes RSX_FSR in the numbering of the renderer that will actually run. */
    r2_renderer_migrate();

    /* R2 perf 1115: [spu-jobs] / [spike] SPU job timing. R2's WWS policy module calls a job at LS 0x35D4
     * (bisled $lr,$5 -> return pc 0x35D8); the runtime default 0x2AA0 is R3's. Image 419 (the R3 job the
     * focus / lane boost default to) means nothing here. SPU_JOB_TIMING=0 turns the timing off. */
    if (!getenv("SPU_WWSJOB_LINK")) _putenv_s("SPU_WWSJOB_LINK", "0x35D8");
    if (!getenv("SPU_JOB_FOCUS")) _putenv_s("SPU_JOB_FOCUS", "0");
    if (!getenv("SPU_JOB_BOOST")) _putenv_s("SPU_JOB_BOOST", "0");
    /* R2's movie player (func_006A531C) asks for the English voice tracks (20 + k) when the system language
     * is 8..11 (Russian, Korean, Chinese), so the file layer's voice fallback must not rename them for those
     * (lang_fallback.h). PS3_LANG_TITLE_ENGLISH=none restores 0.3.0's renaming. */
    if (!getenv("PS3_LANG_TITLE_ENGLISH")) _putenv_s("PS3_LANG_TITLE_ENGLISH", "8-11");
    { g_sjt_link_b = (uint32_t)strtoul(getenv("SPU_WWSJOB_LINK"), 0, 0) & 0x3FFFFu; }
    { const char* e = getenv("PORT_SPU_JOB_TAGS"); if (!(e && *e == '0')) g_port_hitch_job_tag = r2_spu_job_tag; }
    g_port_hitch_site_name = r2_wait_site_name;
    /* Before anything starts: the profile sets env switches that the
     * subsystems read when they first run. R2_PRESET fills in the settings
     * that aren't set explicitly, before the profile reads them. */
    if (const char* preset = getenv("R2_PRESET"); preset && *preset)
        r2ac::apply_preset_env(preset, r2ac::Phase::startup);
    port_cpu_profile_init();
    /* [cores]: the physical-core count (PORT_CORE_SCARCE, PORT_CORE_SCARCE_BELOW), reported at start-up
     * as in the Fall of Man port, where it picks the RSX frame clock's idle wait. */
    (void)ps3_host_core_scarce();
    r2_crash_init();

#ifdef _WIN32
#pragma comment(lib, "winmm.lib")
    /* 1 ms timer resolution. The default granularity is about 15.6 ms, which
     * inflates every shorter wait the title makes and throttles the whole
     * thing. POSIX timers are already fine-grained. */
    timeBeginPeriod(1);
    /* Windows 11 ignores timeBeginPeriod for a process whose window is
     * minimised or covered: the opt-out (PORT_TIMER_NOTHROTTLE) is part of the
     * single power-throttling call in port_cpu_profile_init (cpu_profile.inc). */
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: do not lose prints on a kill */
#endif

    printf("=== ps3recomp game runner ===\n");
#ifdef _WIN32
    port_sys_report();
#endif
    {   /* defaults: r2_hot.cpp, r2_native_jobs.c, r2_native_audio.c, tiled_dma_copy_fast.hpp. edge-zlib
         * (native_edge_zlib.c) is native-jobs kernel 1 (mask bit 1, like R3's kernel bit 0) and water-jobs
         * are kernels 2..4: their state is the effective one from r2_native_jobs.c (R2_NATIVE_JOBS, _MASK,
         * _WATER, PORT_NATIVE_EDGE_ZLIB, AVX2 build). If the entry's own switch is set but overridden by
         * one of the others, the label says so (and is not looked up as a switch).
         * Kernel bits: 0 dd413-xform4, 1 edge-zlib, 2..4 water (7db0 grad/upblend, 4154 phasor), 5 dd413-cull. */
        const int jobs_on = r2nj_effective_mask() != 0;
        const int zlib_on = r2nj_kernel_active(1);
        const int water_on = r2nj_kernel_active(2) && r2nj_kernel_active(3) && r2nj_kernel_active(4);
        auto eff_env = [](const char* env, const char* overridden, int on) {
            const char* e = getenv(env);
            return port_native_parse_bool(e, on) != on ? overridden : env;
        };
        char pcore_sw[96];
        const int pcore_on = !port_pcore_why();
        port_pcore_label(pcore_sw, sizeof pcore_sw, pcore_on, port_pcore_why());
        const port_native_feature k_native[] = {
            { "hot-ppu", "R2_HOT_NATIVE", 1 },
            { "spu-jobs", eff_env("R2_NATIVE_JOBS", "R2_NATIVE_JOBS,overridden", jobs_on), jobs_on }, { "audio-jobs", "R2_NATIVE_AUDIO_JOB", 1 },
            { "edge-zlib", eff_env("PORT_NATIVE_EDGE_ZLIB", "PORT_NATIVE_EDGE_ZLIB,overridden", zlib_on), zlib_on },
            { "detile-fast", "RSX_DETILE_FAST", 1 },
            { "water-jobs", eff_env("R2_NATIVE_JOBS_WATER", "R2_NATIVE_JOBS_WATER,overridden", water_on), water_on },
            { "pcore-pin", pcore_sw, pcore_on },
            { "spu-sched", "PORT_SPU_SCHED", spu_sched_effective() },
            /* RSX_GPU_* (gpu-texdecode/gpu-blit/gpu-texstream): r2rsx_bridge.cpp apply_gpu_offload() prints its own
             * [native] line with the effective state once the yml and the R2_* aliases are applied. */
        };
        port_native_summary(k_native, (int)(sizeof k_native / sizeof k_native[0]));
    }

    if (!alloc_guest_vm()) {
        fprintf(stderr, "ERROR: could not allocate the guest address space\n");
        return 1;
    }
    {   /* PORT_LARGE_PAGES cannot be honoured in this port: the RSX texture cache write-protects guest memory
         * in 4 KB pages (VirtualProtect) to see CPU writes to textures, which a large page cannot do, and the
         * guest space is one pagefile section mapped twice (vm_sudo_base), which would have to be committed whole
         * in large pages. The switch is still read so the [native] line says why it is off. */
        port_large_pages lp = { port_large_pages_env(), 0, 0, 0, NULL };
        if (lp.requested) lp.reason = "texture cache needs 4 KB page protection";
        port_large_pages_summary(&lp);
    }

    uint32_t entry = ppu_load_elf(argv[1]);
    if (!entry) {
        fprintf(stderr, "ERROR: could not load %s\n", argv[1]);
        return 1;
    }

    derive_vfs_root(argv[1]);
    printf("[boot] VFS root: %s\n", ppu_vfs_root);

    /* Resistance port: real title id (BCES00226) from PARAM.SFO, not BLES00000 */
    {
        char sfo[1100];
        snprintf(sfo, sizeof sfo, "%s/PS3_GAME/PARAM.SFO", ppu_vfs_root);
        cellGame_init_from_paramsfo(sfo);
    }

    ppu_recomp_register();   /* lifted function table -> address map */
    ps3_load_prx_modules();  /* this game's lifted system PRX, if it has any */
    ppu_hle_init();          /* firmware import NID -> HLE handlers */
    ppu_sysprx_register();   /* boot-critical CRT */
    ppu_fs_register();       /* cellFs over the game directory */
    lv2_init_syscalls();     /* the lv2 syscall table */

    /* Install the guest-callback hook, then start the frame clock. It no-ops
     * until the title registers its vblank and flip handlers during init. */
    g_ps3_guest_caller = harness_guest_caller;
    /* R2_FPS and RSX_VSYNC=auto are settled before any backend reads them. */
    {
        const double fps = r2_fps_target();
        printf("[boot] frame target %s%.3f fps, RSX_VSYNC=%s\n", fps > 0.0 ? "" : "uncapped ", fps,
               getenv("RSX_VSYNC") ? getenv("RSX_VSYNC") : "(unset)");
    }
    r2_patches_apply();      /* RPCS3 patch.yml data patches (r2_patches.cpp) */
    CreateThread(NULL, 4u * 1024 * 1024, frame_clock, NULL, 0, NULL);

    printf("\n[boot] dispatching entry OPD 0x%08X (stack top 0x%08X)\n\n",
           entry, STACK_TOP);

    int rc = ppu_run(entry, STACK_TOP);
    printf("\n[boot] ppu_run returned %d (entry function unwound)\n", rc);

    /* A title that called sys_process_exit never reaches this line: that path
     * ends in the host exit(). Getting here means the entry function returned
     * instead, so hand back the status the guest published if it published
     * one. Returning a hardcoded 0 would report success for a run that never
     * got anywhere. */
    return g_sys_process_exit_called ? (int)g_sys_process_exit_code : 0;
}
