/* Keyboard keys and the game's controller layout (Resistance 2).
 *
 * Keys are bound to ACTIONS in the launcher (the PS3's default layout: L1 aim,
 * R2 alternate fire, L2 crouch / sprint, ...). The game's Options > Controls
 * screen can pick another layout (presets 1-3, a custom one) and a "separate
 * crouch" setting that moves crouch to L3 and leaves L2 for sprint. The game
 * then maps buttons to actions through that layout, so a key that pressed the
 * default button would do another action. Each key's button is translated:
 * default layout slot for the button -> the button the active layout puts there.
 *
 * Layouts are game words (the game's own button order, game.elf func_002AB274):
 *   0x0001 Triangle 0x0002 Circle 0x0004 Cross 0x0008 Square
 *   0x0010 L2 0x0020 R2 0x0040 L1 0x0080 R1
 *   0x0100 Select 0x0200 Start 0x0400 L3 0x0800 R3
 *   0x1000 Up 0x2000 Right 0x4000 Down 0x8000 Left
 * One 32-bit word per action, 0xA4 bytes per layout (func_002AC9EC). */
#ifndef KBM_LAYOUT_H
#define KBM_LAYOUT_H

#include <stdint.h>

#define KBM_LAYOUT_SLOTS   20
#define KBM_SLOT_CROUCH    6    /* +0x18: crouch (L2, or L3 with "separate crouch") */
#define KBM_SLOT_SPRINT    16   /* +0x40: sprint (L2) */

enum { KBM_ROLE_NONE = 0, KBM_ROLE_CROUCH = 1, KBM_ROLE_SPRINT = 2 };   /* an L2 key's row (controls.txt comment) */

/* the shipped layout, func_002AC9EC: jump, fire, alternate fire, aim, up, down,
 * crouch, separate crouch, melee, map, reload, interact, grenade type,
 * flashlight, down, grenade, sprint, switch weapon, right, down */
static const uint16_t kbm_def_layout[KBM_LAYOUT_SLOTS] = {
    0x0004, 0x0080, 0x0020, 0x0040, 0x1000, 0x4000, 0x0010, 0x0400, 0x0800, 0x8000,
    0x0008, 0x0001, 0x1000, 0x2000, 0x4000, 0x0002, 0x0010, 0x0001, 0x2000, 0x4000,
};

/* CELL_PAD_CTRL_* bit n (cellPad.h) -> game word */
static const uint16_t kbm_sdk_game[16] = {
    0x0100 /* SELECT */, 0x0400 /* L3 */, 0x0800 /* R3 */, 0x0200 /* START */,
    0x1000 /* UP */, 0x2000 /* RIGHT */, 0x4000 /* DOWN */, 0x8000 /* LEFT */,
    0x0010 /* L2 */, 0x0020 /* R2 */, 0x0040 /* L1 */, 0x0080 /* R1 */,
    0x0001 /* TRIANGLE */, 0x0002 /* CIRCLE */, 0x0004 /* CROSS */, 0x0008 /* SQUARE */,
};

static inline uint16_t kbm_game_to_sdk_bit(uint16_t g)
{
    for (int b = 0; b < 16; b++) if (kbm_sdk_game[b] == g) return (uint16_t)(1u << b);
    return 0;
}

/* a usable layout: each action on one button (0 = none), and the main ones set */
static inline int kbm_layout_valid(const uint32_t cur[KBM_LAYOUT_SLOTS])
{
    for (int i = 0; i < KBM_LAYOUT_SLOTS; i++) {
        const uint32_t w = cur[i];
        if (w > 0xFFFF || (w & (w - 1))) return 0;
    }
    return cur[0] && cur[1] && cur[2] && cur[3];
}

/* l2 mask for kbm_layout_translate: which actions the held L2 means */
#define KBM_L2_CROUCH  1
#define KBM_L2_SPRINT  2

static inline uint16_t kbm_slot_sdk(const uint32_t cur[KBM_LAYOUT_SLOTS], int slot)
{
    const uint16_t s = cur[slot] ? kbm_game_to_sdk_bit((uint16_t)cur[slot]) : 0;
    return s ? s : kbm_game_to_sdk_bit(kbm_def_layout[slot]);
}

/* Keyboard buttons (CELL_PAD_CTRL_*) meant with the default layout -> the buttons
 * that do the same actions with layout `cur`. An L2 in `btns` is crouch and / or
 * sprint as `l2` (KBM_L2_*) says: they share L2 by default, not with separate
 * crouch, so a held sprint key and a crouch key each press their own button
 * (0 = crouch). A button used by several actions (d-pad, Triangle) follows the
 * first of them the layout moved. */
static inline uint16_t kbm_layout_translate(uint16_t btns, const uint32_t cur[KBM_LAYOUT_SLOTS], int l2)
{
    uint16_t out = 0;
    for (int b = 0; b < 16; b++) {
        const uint16_t sdk = (uint16_t)(1u << b);
        if (!(btns & sdk)) continue;
        const uint16_t g = kbm_sdk_game[b];
        if (g == 0x0010) {
            if (!(l2 & (KBM_L2_CROUCH | KBM_L2_SPRINT))) l2 = KBM_L2_CROUCH;
            if (l2 & KBM_L2_CROUCH) out |= kbm_slot_sdk(cur, KBM_SLOT_CROUCH);
            if (l2 & KBM_L2_SPRINT) out |= kbm_slot_sdk(cur, KBM_SLOT_SPRINT);
            continue;
        }
        uint16_t to = g;
        for (int i = 0; i < KBM_LAYOUT_SLOTS; i++)
            if (kbm_def_layout[i] == g && cur[i] && cur[i] != g) { to = (uint16_t)cur[i]; break; }
        const uint16_t s = kbm_game_to_sdk_bit(to);
        out |= s ? s : sdk;
    }
    return out;
}

#endif
