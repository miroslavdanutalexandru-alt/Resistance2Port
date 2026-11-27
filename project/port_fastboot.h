/* Developer fast boot for automated test runs (opt-in, default off; tools\fastboot.ps1 sets these):
 *   PORT_FAST_BOOT=1            skip the start-up logo / intro movies (SKIP_INTRO=1).
 *   PORT_FAST_BOOT_CUTSCENES=1  also skip story movies, through the port's own cutscene skip (r2_pcextra.cpp).
 *   PORT_BOOT_LEVEL=<where>     start in that level. The retail game ignores its "-level" option, so the port
 *                               drives the menus with injected pad presses (r2_pcextra.cpp fb_drive_tick):
 *                               auto-save notice, Campaign, Load Level, row <n>, Normal. <where> is a name from
 *                               the table below or the row number in Campaign > Load Level (1 = first).
 *                               "last" = the bottom row.
 *   PORT_BOOT_CHECKPOINT=<cp>   start the picked level at that checkpoint instead: a define_checkpoint name from
 *                               the level's levelconfig.lua (e.g. cp_boss_catwalks1) or its 0-based index
 *                               (r2_pcextra.cpp r2x_boot_checkpoint; logs "[fastboot] start checkpoint ...").
 *   PORT_FAST_BOOT_TRACE=1      log every menu FSCommand once.
 * Once the player is in control, "[fastboot] gameplay ..." is printed (r2_pcextra.cpp). */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <string>

/* "last": the bottom row of Load Level (Down is pressed 60 times; assumes the list does not wrap) */
#define PORT_FASTBOOT_LAST_ROW 999
/* name -> Load Level row (checked on screen) */
static const struct { const char* name; int row; const char* title; } port_fastboot_levels[] = {
    { "iceland", 1, "Holar, Iceland (the opening)" },
    { "chicxulub", PORT_FASTBOOT_LAST_ROW, "Chicxulub, Mexico (the final level: the last Load Level row, not checked on screen)" },
};

static inline int port_fastboot_env_on(const char* name)
{
    const char* e = getenv(name);
    return e && *e && *e != '0';
}

/* PORT_BOOT_LEVEL -> Load Level row 1.. (0 = none) */
static inline int port_fastboot_row(void)
{
    const char* e = getenv("PORT_BOOT_LEVEL");
    if (!e || !*e) return 0;
    std::string v(e);
    const size_t s = v.find_last_of("/\\");                 /* "levels/iceland" works too */
    if (s != std::string::npos) v = v.substr(s + 1);
    for (auto& c : v) c = (char)tolower((unsigned char)c);
    if (v == "last") return PORT_FASTBOOT_LAST_ROW;
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
    if (row == PORT_FASTBOOT_LAST_ROW) fprintf(stderr, "[fastboot] PORT_BOOT_LEVEL: the menus are driven to Campaign > Load Level, last row\n");
    else if (row) fprintf(stderr, "[fastboot] PORT_BOOT_LEVEL: the menus are driven to Campaign > Load Level, row %d\n", row);
    const char* cp = getenv("PORT_BOOT_CHECKPOINT");
    if (cp && *cp && *cp != '0') fprintf(stderr, "[fastboot] PORT_BOOT_CHECKPOINT=%s: the level starts at that checkpoint\n", cp);
}
