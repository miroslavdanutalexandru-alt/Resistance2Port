/* Keyboard keys follow the game's controller layout (kbm_layout.h): presets,
 * "separate crouch" and the crouch / sprint keys that share L2.
 *
 *   clang-cl /I include libs\input\tests\test_kbm_layout.c   (from ps3recomp\)
 */
#include "../kbm_layout.h"
#include "../cellPad.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void preset(uint32_t cur[KBM_LAYOUT_SLOTS], int idx, int sep)
{
    for (int i = 0; i < KBM_LAYOUT_SLOTS; i++) cur[i] = kbm_def_layout[i];
    /* func_002AB0DC */
    if (idx == 1) { cur[2] = 0x0040; cur[3] = 0x0020; }
    if (idx == 2) { cur[1] = 0x0020; cur[2] = 0x0010; cur[3] = 0x0080; cur[6] = cur[16] = 0x0040; }
    if (idx == 3) { cur[2] = 0x0040; cur[3] = 0x0020; cur[6] = cur[16] = 0x0400; cur[7] = 0x0002; cur[15] = 0x0010; }
    if (sep) cur[6] = cur[7];
}

int main(void)
{
    uint32_t cur[KBM_LAYOUT_SLOTS];

    /* shipped layout: nothing moves */
    preset(cur, 0, 0);
    assert(kbm_layout_valid(cur));
    assert(kbm_layout_translate(0xFFFF, cur, KBM_ROLE_NONE) == 0xFFFF);

    /* preset 1: aim key (L1) -> R2, alternate fire key (R2) -> L1 (issue #19) */
    preset(cur, 1, 0);
    assert(kbm_layout_translate(CELL_PAD_CTRL_L1, cur, KBM_ROLE_NONE) == CELL_PAD_CTRL_R2);
    assert(kbm_layout_translate(CELL_PAD_CTRL_R2, cur, KBM_ROLE_NONE) == CELL_PAD_CTRL_L1);
    assert(kbm_layout_translate(CELL_PAD_CTRL_R1 | CELL_PAD_CTRL_CROSS, cur, KBM_ROLE_NONE)
           == (CELL_PAD_CTRL_R1 | CELL_PAD_CTRL_CROSS));

    /* separate crouch: the crouch key presses L3, the sprint key stays on L2 */
    preset(cur, 1, 1);
    assert(kbm_layout_translate(CELL_PAD_CTRL_L2, cur, KBM_ROLE_CROUCH) == CELL_PAD_CTRL_L3);
    assert(kbm_layout_translate(CELL_PAD_CTRL_L2, cur, KBM_ROLE_SPRINT) == CELL_PAD_CTRL_L2);
    assert(kbm_layout_translate(CELL_PAD_CTRL_L2, cur, KBM_ROLE_NONE) == CELL_PAD_CTRL_L3);
    assert(kbm_layout_translate(CELL_PAD_CTRL_R3, cur, KBM_ROLE_NONE) == CELL_PAD_CTRL_R3);      /* melee */
    assert(kbm_layout_translate(CELL_PAD_CTRL_START, cur, KBM_ROLE_NONE) == CELL_PAD_CTRL_START);

    /* preset 2: fire R2, alternate L2, aim R1, crouch / sprint L1 */
    preset(cur, 2, 0);
    assert(kbm_layout_translate(CELL_PAD_CTRL_R1, cur, KBM_ROLE_NONE) == CELL_PAD_CTRL_R2);
    assert(kbm_layout_translate(CELL_PAD_CTRL_R2, cur, KBM_ROLE_NONE) == CELL_PAD_CTRL_L2);
    assert(kbm_layout_translate(CELL_PAD_CTRL_L1, cur, KBM_ROLE_NONE) == CELL_PAD_CTRL_R1);
    assert(kbm_layout_translate(CELL_PAD_CTRL_L2, cur, KBM_ROLE_SPRINT) == CELL_PAD_CTRL_L1);

    /* preset 3: grenade (Circle) -> L2, crouch -> L3 */
    preset(cur, 3, 0);
    assert(kbm_layout_translate(CELL_PAD_CTRL_CIRCLE, cur, KBM_ROLE_NONE) == CELL_PAD_CTRL_L2);
    assert(kbm_layout_translate(CELL_PAD_CTRL_L2, cur, KBM_ROLE_CROUCH) == CELL_PAD_CTRL_L3);

    /* garbage is refused (the caller falls back) */
    preset(cur, 0, 0); cur[4] = 0x1001;
    assert(!kbm_layout_valid(cur));
    preset(cur, 0, 0); cur[1] = 0;
    assert(!kbm_layout_valid(cur));

    printf("test_kbm_layout: ok\n");
    return 0;
}
