/* Resistance 2 PC port: optional single-player cheats (God mode, Infinite ammo,
 * Infinite grenades). All off by default; they only touch the local players.
 *
 *   God mode           every frame the game's own "set_hero_health_min"
 *                      native (func_00034854, the Lua binding at OPD
 *                      0x00D73940) is run with 4 = all four health bars, so
 *                      every local player's health is topped back up to full,
 *                      and the hero's moby is kept non-damageable (what
 *                      set_damageable(moby, false) does: bit 0x8 at moby+0x64),
 *                      so instant-kill attacks don't land either. A hero that is
 *                      dead anyway (scripted kills) is left alone until the
 *                      respawn (PORT_CHEAT_GOD_DEATHFIX, on by default).
 *   Infinite ammo      every frame, for each local player's weapons (the
 *                      weapon lookup func_000F35BC, types 0..46 except the
 *                      grenades) the four ammo counters (primary +0x314/+0x316,
 *                      alt +0x318/+0x31A, int16) are not allowed to drop below the
 *                      highest value seen for that weapon. Ammo kinds 7 / 10
 *                      (infinite) and 9 (no count, heat) at weapon+0x32B / +0x32C
 *                      are left alone; kind 1 also holds the clip (+0x316 / +0x31A).
 *   Infinite grenades  the same for the grenade counts (player+0x8A0 inventory,
 *                      types 21..24, count byte at +0x220 + (type - 21) + 9).
 *
 * Turned on by the launcher / F9 menu rows (PORT_CHEAT_GODMODE, PORT_CHEAT_AMMO,
 * PORT_CHEAT_GRENADES = 1; R2_CHEAT_GOD / R2_CHEAT_AMMO still work, the latter
 * also turns on grenades) or, when PORT_CHEAT_KEYS=1, Ctrl+F1 / F2 / F3
 * (r2_pcextra.cpp). Once any has been on, trophy unlocks are not recorded until
 * the game is restarted (g_trophy_unlocks_blocked, sceNpTrophy.c). In an R2_BENCH
 * run and with PORT_ONLINE on they all stay off. Every change is logged.
 *
 * Runs on the game's main thread at the top of the frame limiter func_00635D28
 * (hook in tools/hooks/r2.json), i.e. between frames. The guest calls run on a
 * scratch frame 0x400 below the guest stack pointer and the whole PPU context is
 * restored afterwards, so the limiter itself sees exactly the registers it would
 * have had. Addresses are for BCES00226 1.00 (the only EBOOT the port supports).
 *
 * First version by Jishnuraj07 (GitHub R2 PR #14). */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "ppu_recomp.h"
#ifdef _WIN32
#include <windows.h>
#endif

extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
extern "C" volatile int g_trophy_unlocks_blocked;   /* sceNpTrophy.c */

extern "C" volatile int g_r2_cheat_god = 0;
extern "C" volatile int g_r2_cheat_ammo = 0;
extern "C" volatile int g_r2_cheat_gren = 0;

static int env_on(const char* n) { const char* e = getenv(n); return e && *e && strcmp(e, "0") != 0; }
/* R2_BENCH runs stay comparable and PORT_ONLINE (RPCN, port_online.c) is never
 * cheated in: no cheats at all (env vars, menu rows and keys are ignored). */
static int cheats_blocked(void)
{
    static int why = -1;
    if (why < 0) why = env_on("R2_BENCH") ? 1 : env_on("PORT_ONLINE") ? 2 : 0;
    return why;
}
/* Once a cheat has been on, trophies stay off until the game is restarted. */
static void block_trophies(void)
{
    if (g_trophy_unlocks_blocked) return;
    g_trophy_unlocks_blocked = 1;
    fprintf(stderr, "[cheat] trophies are off until the game is restarted\n");
}
static const char* const s_name[3] = { "god mode", "infinite ammo", "infinite grenades" };
static volatile int& cheat_flag(int which) { return which == 0 ? g_r2_cheat_god : which == 1 ? g_r2_cheat_ammo : g_r2_cheat_gren; }
/* The old R2_CHEAT_* names (R2 PR #14) become the PORT_CHEAT_* the menu rows and
 * the launcher use. Called before the F9 menu reads them (r2_pcextra.cpp opts_init). */
extern "C" void r2_cheats_init_env(void)
{
    static int done = 0;
    if (done) return;
    done = 1;
    if (env_on("R2_CHEAT_GOD") && !getenv("PORT_CHEAT_GODMODE")) _putenv_s("PORT_CHEAT_GODMODE", "1");
    if (env_on("R2_CHEAT_AMMO")) {
        if (!getenv("PORT_CHEAT_AMMO")) _putenv_s("PORT_CHEAT_AMMO", "1");
        if (!getenv("PORT_CHEAT_GRENADES")) _putenv_s("PORT_CHEAT_GRENADES", "1");
    }
    const int why = cheats_blocked();
    if (why && (env_on("PORT_CHEAT_GODMODE") || env_on("PORT_CHEAT_AMMO") || env_on("PORT_CHEAT_GRENADES")))
        fprintf(stderr, "[cheat] %s: cheats are off\n", why == 1 ? "benchmark run" : "online (PORT_ONLINE)");
}
/* 0 = God mode, 1 = Infinite ammo, 2 = Infinite grenades; on: 1 / 0, -1 = toggle.
 * Returns the new state, -1 in a benchmark run, -2 with online on. Any thread. */
extern "C" int r2_cheat_set(int which, int on)
{
    if (which < 0 || which > 2) return 0;
    r2_cheats_init_env();
    if (const int why = cheats_blocked()) return -why;
    volatile int& v = cheat_flag(which);
    if (on < 0) on = !v;
    on = on ? 1 : 0;
    if (on) block_trophies();
    if (v != on) fprintf(stderr, "[cheat] %s %s\n", s_name[which], on ? "on" : "off");
    v = on;
    return on;
}
extern "C" int r2_cheat_toggle(int which) { return r2_cheat_set(which, -1); }

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

/* grenades (func_00034488 / func_000E7F20): player+0x8A0 inventory, count byte
 * at +0x220 + (type - 21) + 9 for the grenade types 21..24 */
static void grenades_player(uint32_t player)
{
    const uint32_t f = 0x8A0 + 0x220 + 9;
    if (s_diag && !s_gdiag && (s_gdiag = 1))
        fprintf(stderr, "[cheat] player 0x%08X grenades %u %u %u %u\n", player, vm_read8(player + f),
                vm_read8(player + f + 1), vm_read8(player + f + 2), vm_read8(player + f + 3));
    for (uint32_t k = 0; k < 4; k++) hold8(player, f + k);
}
static void ammo_player(ppu_context* c, uint32_t player)
{
    for (uint32_t type = 0; type < 0x2F; type++) {
        if (type >= 21 && type <= 24) continue;   /* grenades: grenades_player */
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
 * Only a bit we cleared is set again when God mode is turned off, and only on a
 * moby that is still a local player's hero then. The table is a ring: when a long
 * session has gone through more hero mobys than it holds, the oldest is forgotten
 * (it belonged to a level that has been left). */
struct Inv { uint32_t moby; int cleared; };
static Inv s_inv[16];
static int s_ninv = 0, s_inv_next = 0;
static const int INV_N = (int)(sizeof s_inv / sizeof s_inv[0]);
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
            if (s_ninv < INV_N) k = s_ninv++;
            else { k = s_inv_next; s_inv_next = (s_inv_next + 1) % INV_N; }
            s_inv[k] = { moby, 0 };
            fprintf(stderr, "[cheat] god mode: hero moby 0x%08X (player 0x%08X) flags %08X -> %08X\n", moby, player, cur, cur & ~0x8u);
        }
        s_inv[k].cleared = 1;
        vm_write32(a, cur & ~0x8u);
    } else if (k < s_ninv && s_inv[k].cleared) {
        s_inv[k].cleared = 0;
        vm_write32(a, cur | 0x8u);
        fprintf(stderr, "[cheat] god mode off: hero moby 0x%08X flags %08X -> %08X\n", moby, cur, cur | 0x8u);
    }
}

/* God mode, part 3: a hero that is already dead is left alone. Scripted instant
 * kills (Lua kill(HALE): the Leviathan smash and the kill zones in Chicago
 * seg16b_enemy_segment.lua, falls...) go through func_0002CE94 and don't care
 * about the damageable bit, so the hero dies anyway; topping the dead hero's
 * health back up every frame then left the death half done: camera on the body,
 * no fade, no checkpoint reload. While a local hero is dead there is no health
 * refill (and the damageable bit isn't touched) so the game's own death ->
 * respawn sequence runs; God mode carries on once the hero is alive again.
 * PORT_CHEAT_GOD_DEATHFIX=0 gives the old behaviour (refill even when dead). */
static int god_deathfix(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("PORT_CHEAT_GOD_DEATHFIX"); v = !(e && *e && strcmp(e, "0") == 0); }
    return v;
}
static void god_death(int god, int ndead)
{
    static int was = 0, told_long = 0;
    static unsigned long long t0 = 0;
#ifdef _WIN32
    const unsigned long long now = GetTickCount64();
#else
    const unsigned long long now = 0;
#endif
    if (god && ndead && !was) {
        t0 = now; told_long = 0;
        fprintf(stderr, god_deathfix() ? "[cheat] god mode: hero dead (scripted kill), no health refill until the respawn\n"
                                       : "[cheat] god mode: hero dead (scripted kill), health refill goes on (PORT_CHEAT_GOD_DEATHFIX=0)\n");
    } else if (god && ndead && !told_long && now - t0 > 30000) {
        told_long = 1;
        fprintf(stderr, "[cheat] god mode: hero still dead after %.0f s\n", (now - t0) / 1000.0);
    } else if (!ndead && was) {
        fprintf(stderr, "[cheat] god mode: hero %s after %.1f s\n", god ? "alive again" : "no longer tracked (god mode off)", (now - t0) / 1000.0);
    }
    was = god && ndead;
}

extern "C" void r2_cheats_frame(ppu_context* ctx)
{
    static int init = 0;
    if (!init) {
        init = 1;
        r2_cheats_init_env();
        if (!cheats_blocked()) {
            if (env_on("PORT_CHEAT_GODMODE")) r2_cheat_set(0, 1);
            if (env_on("PORT_CHEAT_AMMO")) r2_cheat_set(1, 1);
            if (env_on("PORT_CHEAT_GRENADES")) r2_cheat_set(2, 1);
            if (g_r2_cheat_god || g_r2_cheat_ammo || g_r2_cheat_gren)
                fprintf(stderr, "[cheat] at start: god %d, ammo %d, grenades %d\n", g_r2_cheat_god, g_r2_cheat_ammo, g_r2_cheat_gren);
        }
    }
    if (cheats_blocked()) return;
    static int hold_was = 0, god_was = 0, restore = 0;
    const int god = g_r2_cheat_god, ammo = g_r2_cheat_ammo, gren = g_r2_cheat_gren;
    if (god || ammo || gren) block_trophies();
    const int hold = ammo || gren;
    if (hold && !hold_was) { s_nhwm = 0; s_ndiag = 0; s_gdiag = 0; s_diag = 1; }   /* fresh start every time it is turned on */
    hold_was = hold;
    if (god_was && !god) restore = 1;    /* turned off: give back the invulnerability bits (once a player exists) */
    if (god) restore = 0;
    god_was = god;
    if (!god && !hold && !restore) return;
    if (g_trampoline_fn) return;         /* never expected at function entry; don't disturb it */

    static ppu_context save;             /* main thread only */
    save = *ctx;
    ppu_context* c = ctx;
    c->gpr[1] = ((save.gpr[1] & 0xFFFFFFFFull) - 0x400) & ~0xFull;
    const uint32_t iter = (uint32_t)c->gpr[1] + 0x100;   /* above the callees' frames, below the limiter's */

    gcall(c, func_0071DE90, iter);                        /* local player iterator: begin */
    int np = 0, ndead = 0;
    for (int guard = 0; guard < 8 && (gcall(c, func_0071DCD0, iter) & 0xFF); guard++) {
        const uint32_t player = (uint32_t)gcall(c, func_0011C93C, iter);
        if (player) {
            np++;
            /* the game's own dead test (Lua is_dead: func_0002D01C -> func_0002CFB0
             * -> func_00A776BC([moby+0x40]), [moby+0x40] being this player) */
            const int dead = god && (gcall(c, func_00A776BC, player) & 0xFF);
            if (dead) ndead++;
            if ((god && !(dead && god_deathfix())) || restore) {
                const uint32_t ent = vm_read32(iter);
                if (ent) god_moby((uint32_t)gcall(c, func_0071DC64, ent), player, god);
            }
            if (ammo) ammo_player(c, player);
            if (gren) grenades_player(player);
        }
        gcall(c, func_0071DD34, iter);                    /* next */
    }
    if (restore && np) { restore = 0; s_ninv = 0; s_inv_next = 0; }
    god_death(god, ndead);
    if (god && !(ndead && god_deathfix())) gcall(c, func_00034854, 4);
    static int np_was = -1;
    if (np != np_was) { fprintf(stderr, "[cheat] %d local player(s)\n", np); np_was = np; }

    *ctx = save;
}
