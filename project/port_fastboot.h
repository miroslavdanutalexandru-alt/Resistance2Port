/* Developer fast boot for automated test runs (opt-in, default off; tools\fastboot.ps1 sets these):
 *   PORT_FAST_BOOT=1            skip the start-up logo / intro movies (SKIP_INTRO=1).
 *   PORT_FAST_BOOT_CUTSCENES=1  also skip story movies, through the port's own cutscene skip (r2_pcextra.cpp).
 *   PORT_BOOT_LEVEL=<where>     start in that level. The retail game ignores its "-level" option, so the port
 *                               drives the menus with injected pad presses (r2_pcextra.cpp fb_drive_tick):
 *                               auto-save notice, Campaign, Load Level (the row before Cinematics, the last of the
 *                               visible items, counted from the sublist's genericItem tweens; the screen it opens is
 *                               checked: CampaignLoad, else Back and the row above / below, at most twice),
 *                               checkpoint row <n> of the first level (Holar), Normal. <where> is a name from the
 *                               table below or a checkpoint row of the first level (1 = first). Other levels are
 *                               reached by content, not by menu position: the game's "LoadCheckpoint level:cp:diff"
 *                               start (func_0012EC44) is redirected to the level of the table entry's anchor
 *                               checkpoint, found by name in the game's level/checkpoint table.
 *   PORT_BOOT_CHECKPOINT=<cp>   start at that checkpoint instead: a define_checkpoint name from the levelconfig.lua
 *                               (e.g. cp_boss_catwalks1; the level is found by it) or a 0-based index in the level
 *                               (r2_pcextra.cpp r2x_boot_checkpoint; logs "[fastboot] start checkpoint ...").
 *   PORT_FAST_BOOT_TRACE=1      log every menu FSCommand once.
 * Once the player is in control, "[fastboot] gameplay ..." is printed (r2_pcextra.cpp). */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <string>

/* name -> Load Level checkpoint row of the first level, and the anchor checkpoint (a define_checkpoint name only
 * that level has) whose level the start is redirected to (null = the first level itself) */
static const struct { const char* name; int row; const char* anchor; const char* title; } port_fastboot_levels[] = {
    { "iceland", 1, nullptr, "Holar, Iceland (the opening)" },
    { "chicxulub", 1, "cp_boss_catwalks1", "Chicxulub, Mexico (the final level)" },
};

static inline int port_fastboot_env_on(const char* name)
{
    const char* e = getenv(name);
    return e && *e && *e != '0';
}

static inline std::string port_fastboot_level_name(void)
{
    if (!port_fastboot_env_on("PORT_FAST_BOOT")) return std::string();   /* dev tooling: PORT_FAST_BOOT=1 only */
    const char* e = getenv("PORT_BOOT_LEVEL");
    std::string v(e ? e : "");
    const size_t s = v.find_last_of("/\\");                 /* "levels/iceland" works too */
    if (s != std::string::npos) v = v.substr(s + 1);
    for (auto& c : v) c = (char)tolower((unsigned char)c);
    return v;
}
/* PORT_BOOT_LEVEL -> its anchor checkpoint name, or null */
static inline const char* port_fastboot_anchor(void)
{
    const std::string v = port_fastboot_level_name();
    for (auto& l : port_fastboot_levels) if (v == l.name) return l.anchor;
    return nullptr;
}
/* PORT_BOOT_LEVEL -> Load Level row 1.. (0 = none) */
static inline int port_fastboot_row(void)
{
    if (!port_fastboot_env_on("PORT_FAST_BOOT")) return 0;
    const char* e = getenv("PORT_BOOT_LEVEL");
    if (!e || !*e) return 0;
    const std::string v = port_fastboot_level_name();
    for (auto& l : port_fastboot_levels) if (v == l.name) return l.row;
    if (!v.empty() && v.find_first_not_of("0123456789") == std::string::npos && atoi(v.c_str()) >= 1) return atoi(v.c_str());
    fprintf(stderr, "[fastboot] PORT_BOOT_LEVEL=%s: unknown (a name such as iceland, or a Load Level row number)\n", e);
    return 0;
}

static inline void port_fastboot_putenv(const char* k, const char* v)
{
#ifdef _WIN32
    std::string s = std::string(k) + "=" + v;
    _putenv(s.c_str());
#else
    setenv(k, v, 1);
#endif
}

/* main(), before the guest starts */
static inline void port_fastboot_init(void)
{
    if (port_fastboot_env_on("PORT_FAST_BOOT")) {
        port_fastboot_putenv("SKIP_INTRO", "1");
        fprintf(stderr, "[fastboot] PORT_FAST_BOOT=1: intro movies skipped%s\n",
                port_fastboot_env_on("PORT_FAST_BOOT_CUTSCENES") ? ", cutscenes skipped (PORT_FAST_BOOT_CUTSCENES=1)" : "");
    }
    const int row = port_fastboot_row();
    const char* an = port_fastboot_anchor();
    if (row) fprintf(stderr, "[fastboot] PORT_BOOT_LEVEL=%s: the menus are driven to Campaign > Load Level, checkpoint row %d%s%s%s\n",
                     getenv("PORT_BOOT_LEVEL"), row, an ? ", then the start is redirected to the level of " : "", an ? an : "",
                     an ? " (found by name)" : "");
    const char* cp = getenv("PORT_BOOT_CHECKPOINT");
    if (port_fastboot_env_on("PORT_FAST_BOOT") && cp && *cp && *cp != '0') fprintf(stderr, "[fastboot] PORT_BOOT_CHECKPOINT=%s: the level starts at that checkpoint\n", cp);
}
