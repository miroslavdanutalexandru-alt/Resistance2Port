/* Resistance 2 PC port: cheats (God mode, Infinite ammo).
 *
 *   Ctrl+F1   God mode       every frame the game's own "set_hero_health_min"
 *                            native (func_00034854, the Lua binding at OPD
 *                            0x00D73940) is run with 4 = all four health bars, so
 *                            every local player's health is topped back up to full,
 *                            and the hero's moby is kept non-damageable (what
 *                            set_damageable(moby, false) does: bit 0x8 at moby+0x64),
 *                            so instant-kill attacks don't land either.
 *   Ctrl+F2   Infinite ammo  every frame, for each local player's weapons (the
 *                            weapon lookup func_000F35BC, types 0..46) the four
 *                            ammo counters (primary +0x314/+0x316, alt +0x318/+0x31A,
 *                            int16) and the grenade counts (player+0x8A0 inventory,
 *                            types 21..24, slot type-21) are not allowed to drop below the highest
 *                            value seen for that weapon. Ammo kinds 7 / 10 (infinite)
 *                            and 9 (no count, heat) at weapon+0x32B / +0x32C are left
 *                            alone; kind 1 also holds the clip (+0x316 / +0x31A).
 *
 *   R2_CHEAT_GOD=1 / R2_CHEAT_AMMO=1 turn them on at start. Both are off by default.
 *
 * Runs on the game's main thread at the top of the frame limiter func_00635D28
 * (hook in tools/hooks/r2.json), i.e. between frames. The guest calls run on a
 * scratch frame 0x400 below the guest stack pointer and the whole PPU context is
 * restored afterwards, so the limiter itself sees exactly the registers it would
 * have had. Addresses are for BCES00226 1.00 (the only EBOOT the port supports). */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "ppu_recomp.h"
#ifdef _WIN32
#include <windows.h>
#endif

extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);

extern "C" volatile int g_r2_cheat_god = 0;
extern "C" volatile int g_r2_cheat_ammo = 0;

static const uint64_t R2_TOC = 0x00DED008ULL;

static uint64_t gcall(ppu_context* c, void (*f)(ppu_context*), uint64_t a, uint64_t b = 0, uint64_t d = 0)
{
    c->gpr[2] = R2_TOC;
    c->gpr[3] = a; c->gpr[4] = b; c->gpr[5] = d;
    c->lr = 0x00635D28ULL;
    f(c);
    while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf((void*)c); }
    return c->gpr[3] & 0xFFFFFFFFull;
}

/* highest value seen per (weapon / player, its config, field) */
struct Hwm { uint32_t obj, cfg; uint16_t field; int16_t hi; };
static Hwm s_hwm[1024];
static int s_nhwm = 0;
static int s_diag = 0, s_ndiag = 0, s_gdiag = 0;
static uint32_t s_diagw[64];
static int16_t* hwm_slot(uint32_t obj, uint32_t cfg, uint16_t field, int16_t cur)
{
    for (int i = 0; i < s_nhwm; i++)
        if (s_hwm[i].obj == obj && s_hwm[i].cfg == cfg && s_hwm[i].field == field) return &s_hwm[i].hi;
    if (s_nhwm >= (int)(sizeof s_hwm / sizeof s_hwm[0])) s_nhwm = 0;   /* full: start over */
    s_hwm[s_nhwm] = { obj, cfg, field, cur };
    return &s_hwm[s_nhwm++].hi;
}
static void hold16(uint32_t obj, uint32_t cfg, uint16_t field)
{
    const int16_t cur = (int16_t)vm_read16(obj + field);
    if (cur < 0) return;
    int16_t* hi = hwm_slot(obj, cfg, field, cur);
    if (cur > *hi) *hi = cur;
    else if (cur < *hi) vm_write16(obj + field, (uint16_t)*hi);
}
static void hold8(uint32_t obj, uint32_t field)
{
    const int16_t cur = (int16_t)vm_read8(obj + field);
    int16_t* hi = hwm_slot(obj, 0xFFFFFFFFu, (uint16_t)(field & 0xFFFF), cur);
    if (cur > *hi) *hi = cur;
    else if (cur < *hi) vm_write8(obj + field, (uint8_t)*hi);
}
/* ammo kind (func_004D2D68 / func_004D2DCC): 1 = clip +0x316 plus reserve +0x314,
 * 7 / 10 = infinite, 9 = no count (heat), anything else = the single count +0x314 */
static int counted_kind(uint8_t k) { return k != 7 && k != 9 && k != 10; }

static void ammo_player(ppu_context* c, uint32_t player)
{
    for (uint32_t type = 0; type < 0x2F; type++) {
        if (type >= 21 && type <= 24) {   /* grenades (func_00034488 / func_000E7F20): player+0x8A0 inventory,
                                             count byte at +0x220 + (type - 21) + 9 */
            const uint32_t f = 0x8A0 + 0x220 + (type - 21) + 9;
            if (s_diag && !s_gdiag && type == 21 && (s_gdiag = 1))
                fprintf(stderr, "[cheat] player 0x%08X grenades %u %u %u %u\n", player, vm_read8(player + f),
                        vm_read8(player + f + 1), vm_read8(player + f + 2), vm_read8(player + f + 3));
            hold8(player, f);
            continue;
        }
        const uint32_t h = (uint32_t)gcall(c, func_000F35BC, player, type);
        if (!h) continue;
        const uint32_t w = (uint32_t)gcall(c, func_0018C464, h);
        if (!w) continue;
        const uint32_t cfg = vm_read32(w + 0x270);
        if (!cfg) continue;
        const uint8_t kp = vm_read8(w + 0x32B), ka = vm_read8(w + 0x32C);
        if (s_diag && s_ndiag < 64) {
            bool seen = false;
            for (int i = 0; i < s_ndiag; i++) if (s_diagw[i] == w) seen = true;
            if (!seen) {
                s_diagw[s_ndiag++] = w;
                fprintf(stderr, "[cheat] player 0x%08X type %u weapon 0x%08X kind %u/%u ammo %d %d alt %d %d (max %u/%u)\n",
                        player, type, w, kp, ka, (int16_t)vm_read16(w + 0x314), (int16_t)vm_read16(w + 0x316),
                        (int16_t)vm_read16(w + 0x318), (int16_t)vm_read16(w + 0x31A), vm_read16(cfg + 0x4), vm_read16(cfg + 0x6C));
            }
        }
        if (counted_kind(kp)) { hold16(w, cfg, 0x314); if (kp == 1) hold16(w, cfg, 0x316); }
        if (counted_kind(ka)) { hold16(w, cfg, 0x318); if (ka == 1) hold16(w, cfg, 0x31A); }
    }
}

/* God mode, part 2: the hero's moby is made non-damageable, as the game's own
 * Lua set_damageable(moby, false) does (func_000606EC: bit 0x8 of the flag word
 * at moby+0x64). The moby comes from the local-player iterator (func_0071DC64 on
 * the iterator's entry, the same lookup func_0011C93C does before it returns
 * [moby+0x40]); it is only touched when [moby+0x40] is that player. This stops
 * instant-kill attacks (the Chameleon grab) that the health refill can't undo.
 * Only a bit we cleared is set again when God mode is turned off. */
struct Inv { uint32_t moby; int cleared; };
static Inv s_inv[8];
static int s_ninv = 0;
static void god_moby(uint32_t moby, uint32_t player, int on)
{
    if (!moby || vm_read32(moby + 0x40) != player) {
        static int told = 0;
        if (on && !told++) fprintf(stderr, "[cheat] god mode: hero moby not found (moby 0x%08X, player 0x%08X, [moby+0x40] 0x%08X)\n",
                                   moby, player, moby ? vm_read32(moby + 0x40) : 0);
        return;
    }
    const uint32_t a = moby + 0x64;
    int k = 0; while (k < s_ninv && s_inv[k].moby != moby) k++;
    const uint32_t cur = vm_read32(a);
    if (on) {
        if (!(cur & 0x8u)) return;
        if (k == s_ninv) {
            if (s_ninv >= 8) return;
            s_inv[s_ninv++] = { moby, 0 };
            fprintf(stderr, "[cheat] god mode: hero moby 0x%08X (player 0x%08X) flags %08X -> %08X\n", moby, player, cur, cur & ~0x8u);
        }
        s_inv[k].cleared = 1;
        vm_write32(a, cur & ~0x8u);
    } else if (k < s_ninv && s_inv[k].cleared) {
        vm_write32(a, cur | 0x8u);
        fprintf(stderr, "[cheat] god mode off: hero moby 0x%08X flags %08X -> %08X\n", moby, cur, cur | 0x8u);
    }
}

extern "C" void r2_cheats_frame(ppu_context* ctx)
{
    static int init = 0;
    if (!init) {
        init = 1;
        const char* e = getenv("R2_CHEAT_GOD");  if (e && *e && *e != '0') g_r2_cheat_god = 1;
        e = getenv("R2_CHEAT_AMMO");             if (e && *e && *e != '0') g_r2_cheat_ammo = 1;
        if (g_r2_cheat_god || g_r2_cheat_ammo)
            fprintf(stderr, "[cheat] at start: god %d, ammo %d\n", g_r2_cheat_god, g_r2_cheat_ammo);
    }
    static int ammo_was = 0, god_was = 0;
    const int god = g_r2_cheat_god, ammo = g_r2_cheat_ammo;
    if (ammo && !ammo_was) { s_nhwm = 0; s_ndiag = 0; s_gdiag = 0; s_diag = 1; }   /* fresh start every time it is turned on */
    ammo_was = ammo;
    const int god_off = god_was && !god;   /* turned off this frame: give back the invulnerability bits */
    if (!god && !ammo && !god_off) return;
    if (g_trampoline_fn) return;         /* never expected at function entry; don't disturb it */
    god_was = god;

    static ppu_context save;             /* main thread only */
    save = *ctx;
    ppu_context* c = ctx;
    c->gpr[1] = ((save.gpr[1] & 0xFFFFFFFFull) - 0x400) & ~0xFull;
    const uint32_t iter = (uint32_t)c->gpr[1] + 0x100;   /* above the callees' frames, below the limiter's */

    if (god) gcall(c, func_00034854, 4);

    gcall(c, func_0071DE90, iter);                        /* local player iterator: begin */
    int np = 0;
    for (int guard = 0; guard < 8 && (gcall(c, func_0071DCD0, iter) & 0xFF); guard++) {
        const uint32_t player = (uint32_t)gcall(c, func_0011C93C, iter);
        if (player) {
            np++;
            if (god || god_off) {
                const uint32_t ent = vm_read32(iter);
                if (ent) god_moby((uint32_t)gcall(c, func_0071DC64, ent), player, god);
            }
            if (ammo) ammo_player(c, player);
        }
        gcall(c, func_0071DD34, iter);                    /* next */
    }
    if (god_off) s_ninv = 0;
    static int np_was = -1;
    if (np != np_was) { fprintf(stderr, "[cheat] %d local player(s)\n", np); np_was = np; }

    *ctx = save;
}
