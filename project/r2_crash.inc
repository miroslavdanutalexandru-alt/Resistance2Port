/* Resistance port: crash and hang reports for play-testing.
 *
 * The work is done by port_crash.h (shared by the R1/R2/R3 ports): a crash
 * writes logs\crash\R2_<time>_crash.dmp (small minidump) and R2_<time>_crash.txt
 * (exception, faulting module + offset, lifted guest function, every thread's
 * native stack, the end of run.log, the settings), adds a line to
 * logs\crash\crashes.txt and lets the process die as before (after the filter
 * that was installed before it). A hang -- the game loop, r2_game_frames,
 * counted in the stubs.cpp frame limiter, stops -- writes R2_<time>_hang.dmp/.txt
 * once per stall and keeps running.
 *   R2_CRASH_DUMPS=0 / PORT_CRASH_DUMPS=0          both off
 *   R2_CRASH_DUMPS=full (or CRASH_DUMP_FULL=1)      full-memory dumps (several GB)
 *   PORT_HANG_DUMP=1   strict watchdog: 20 s (PORT_HANG_S) without a game frame,
 *                      or without an RSX flip while frames run (not when paused)
 *   logs\hang.now      create it while the game runs: one hang report now (a
 *                      stalled objective / script; lists every guest thread's
 *                      priority, syscall + wait object, call site)
 *   PORT_HANG_DUMP=0   no watchdog; unset: game frames only, R2_HANG_S (90) s
 * vm_base is declared above; VM_SIZE is defined further down, hence the literal. */
#ifdef _WIN32
#include "port_crash.h"
#define PORT_FRAMETIME_IMPL   /* frame-time recorder: logs\frametimes_*.txt, marker key */
#include "port_frametime.h"

extern "C" volatile LONG r2_game_frames = 0;   /* stubs.cpp frame limiter */
extern "C" volatile int g_r2pc_paused;         /* r2_pcport.cpp: focus pause / quit prompt */
extern "C" volatile long g_port_pause_gen;     /* bumped on every pause / resume (stubs.cpp default 0) */
extern "C" uint32_t ppu_prof_resolve_host(void* ra);   /* ppu_loader.cpp: host pc -> lifted function */
extern "C" int r2rsx_enabled(void);
extern "C" uint64_t r2rsx_last_flip_time(void);
extern "C" void rsx_null_refresh_restore_on_crash(void);   /* rsx_null_backend.c: PORT_REFRESH_SWITCH */

static uint64_t r2c_frames(void) { return (uint64_t)(ULONG)r2_game_frames; }
static uint64_t r2c_rsx(void) { return r2rsx_enabled() ? r2rsx_last_flip_time() : 0; }
static int r2c_paused(void) { return g_r2pc_paused != 0; }
/* guest PPU state of the faulting thread (ppu_loader.cpp); PORT_CRASH_EXTRA=0 turns it off */
extern "C" int ppu_crash_guest_state(uint64_t*, uint32_t*, uint32_t*, uint32_t*, uint32_t*, uint32_t*);
static void r2c_guest(PortGuestState* g)
{
    g->valid = ppu_crash_guest_state(&g->thread_id, &g->lr, &g->sp, &g->syscall_inflight,
                                     &g->syscall_last, &g->callsite);
}

extern "C" void ppu_hang_guest_threads(void (*emit)(const char*));   /* sys_ppu_thread.c */
extern "C" void spu_hang_lanes(void (*emit)(const char*));           /* lv2_register.c */
static void r2c_guest_threads(void (*emit)(const char*))
{
    ppu_hang_guest_threads(emit);
    emit("-- spu threads --");
    spu_hang_lanes(emit);
}

/* R3 #33 (same renderer here): an RPCS3 renderer thread (rsx::thread, ...) that dies of a fatal error -- e.g. out
 * of video memory -- used to leave the game frozen on its last frame, with no report (focus pauses kept resetting
 * the hang watchdog). Utilities/Thread.cpp calls this right after logging the error: write the crash report now,
 * tell the player what happened and close the game. Other RPCS3 threads keep the old behaviour. */
#include "r2_autoconfig.h"   /* r2ac::detect_gpu (self-contained, #pragma once; main.cpp includes it again below) */
extern "C" void (*g_port_thread_fatal_hook)(const char* thread_name, const char* reason);
extern "C" void* rsx_null_backend_get_hwnd(void);
static void r2_thread_fatal(const char* thread_name, const char* reason)
{
    if (!thread_name || !(strstr(thread_name, "rsx") || strstr(thread_name, "RSX"))) return;
    static volatile LONG s_once = 0;
    if (InterlockedExchange(&s_once, 1)) { Sleep(INFINITE); }   /* a second dying thread waits for the first to exit */
    if (!reason) reason = "?";
    const bool vram = strstr(reason, "0xfffffffe") || strstr(reason, "OUT_OF_DEVICE_MEMORY") ||
                      (strstr(reason, "memory.cpp") && strstr(reason, "Not enough memory"));
    const DWORD code = vram ? 0xE0000E01u : 0xE0000E02u;
    static char how[640];
    snprintf(how, sizeof how, "%s: renderer thread '%s' died: %.520s", vram ? "out of video memory" : "renderer fatal error",
             thread_name, reason);
    for (char* p = how; *p; ++p) if (*p == '\r' || *p == '\n') *p = ' ';   /* one line in crashes.txt */
    port_crash::crash_here(how, code);

    rsx_null_refresh_restore_on_crash();
    ClipCursor(NULL);
    if (HWND w = (HWND)rsx_null_backend_get_hwnd()) ShowWindowAsync(w, SW_MINIMIZE);
    char msg[1024];
    if (vram) {
        r2ac::Hardware hw;
        r2ac::detect_gpu(hw);
        char gpu[96] = "";
        if (hw.vram_gb > 0.1) snprintf(gpu, sizeof gpu, " (%.0f GB on this GPU)", hw.vram_gb);
        snprintf(msg, sizeof msg,
                 "Resistance 2 has to close: the graphics card ran out of video memory%s.\n\n"
                 "In the launcher, set Render resolution to 1280 x 720 (or pick a lower preset), "
                 "then start the game again.%s", gpu,
                 port_crash::s_on ? "\n\nA report was saved in logs\\crash." : "");
    } else {
        snprintf(msg, sizeof msg,
                 "Resistance 2 has to close: the renderer hit an error it cannot recover from.\n\n%.360s%s", reason,
                 port_crash::s_on ? "\n\nA report was saved in logs\\crash. Please include it and logs\\run.log when you report this." : "");
    }
    fprintf(stderr, "[crash] %s: closing the game\n", vram ? "out of video memory" : "renderer fatal error");
    fflush(stderr);
    MessageBoxA(NULL, msg, "Resistance 2", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
    TerminateProcess(GetCurrentProcess(), code);
}
static void r2_crash_init(void)
{
    PortCrashConfig c = {};
    c.tag = "R2";
    c.title = "Resistance 2";
    c.env_prefix = "R2";
    c.main_progress = r2c_frames;
    c.main_start_min = 1;
    c.rsx_progress = r2c_rsx;
    c.paused = r2c_paused;
    c.vm_base = &vm_base;
    c.vm_size = 0x100010000ull;   /* VM_SIZE */
    c.guest_resolve = ppu_prof_resolve_host;
    c.before_report = rsx_null_refresh_restore_on_crash;
    c.guest_state = r2c_guest;
    c.guest_threads = r2c_guest_threads;
    c.after_report = port_ft_on_report;
    port_crash::init(c);
    g_port_thread_fatal_hook = r2_thread_fatal;
    PortFtConfig f = {};
    f.title = c.title;
    f.paused = r2c_paused;
    f.pause_gen = [](void) -> long { return g_port_pause_gen; };
    port_ft_init(f);
}
#else
extern "C" volatile LONG r2_game_frames = 0;
static void r2_crash_init(void) {}
#endif