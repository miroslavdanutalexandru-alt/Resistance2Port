/* Resistance 2 PC port: the PC settings as pages of the game's own Audio/Video options screen.
 *
 * scaleform/avoptions.swf (in packed/game/global_cached.psarc) is patched at start-up
 * (r2_swfpatch.cpp) and given to the game through the mods folder (r2_mods.cpp, built-in "pc_menu"
 * overlay). The PC pages swap the list's nine rows for their own; the rows use the game's own
 * scroll widget, font and sounds. With R2_MENU_ROWS=1 (the default) scaleform/mainmenu.swf gets an
 * Options > PC SETTINGS entry (after Audio/Visual) that opens the screen on a category list
 * (PC DISPLAY, PC GRAPHICS, PC EFFECTS, PC CONTROLS & HUD, PC AUDIO & LANGUAGE -- the same
 * categories as R1 / R3); Enter opens a category's page, L1 / R1 page through all the pages, Back
 * returns from a page to the category list and from the list to the main menu. With
 * R2_MENU_ROWS=2, or when that patch cannot be made, L1 / R1 on Options > Audio/Visual lead to the
 * category list (Back: list -> Audio/Visual). The movie talks to us
 * through the options FSCommand handler func_002D23C4 (cmd r3, args r4, movie r5):
 *
 *   "PcStart"                     the screen opens: _root.pcStart = [from the PC SETTINGS entry, the entry exists]
 *   "PcGet" <page>                (1..n, not the category list) fills _root.optionsArray_Values with the current choice indices
 *   "PcSet" "<page>:<row>:<index>" a change, applied through the F10 menu code (r2_pcport.cpp /
 *                                 r2_pcextra.cpp: settings.ini, settings.bat, live where possible)
 *   "PcHint"                      fills _root.pcHintC with the page keys ("RMB / LMB", "LB / RB",
 *                                 "L1 / R1") as character codes: L1 / R1 named for the device in use
 *
 * The entry's SetScreen "PcSettings" becomes "AVOptions" at the game's generic FSCommand
 * processor func_00769378 (r2_menu_rows_screen).
 *
 * In game, scaleform/pausemenu.swf gets a PAUSE > PC SETTINGS entry (after OPTIONS; mode 1). It
 * opens the pause menu's own options panel (the same OptionsMenu list as its OPTIONS) on the same
 * category list and pages; its FSCommands reach func_002D23C4 through the pause screens' handlers
 * (func_002D4DD0 campaign, func_002CE148 co-op), so "PcGet" / "PcSet" work the same. Nothing of
 * the F10 PC menu is shown on this path (F10 itself stays a separate quick menu).
 *
 * R2_MENU_ROWS=0 turns all of it off (the original movies, no handler).
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include "ppu_recomp.h"
#include "r2_swfpatch.h"

void func_00A93244(ppu_context* ctx);   /* GFxMovie::SetVariableArray(r3 movie, r4 name, r5 index, r6 values, r7 count, r8 set type) */

extern "C" {
extern PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
/* r2_pcport.cpp (F10 PC menu) */
int pcm_opt_find(const char* key);
int pcm_opt_value(int i, char* out, int n);
void pcm_opt_set_value(int i, const char* v);
int pcm_ini_get(const char* key, char* out, int n);
void pcm_ini_set(const char* key, const char* v);
void pcm_bat_set(const char* env, const char* v);
/* r2_pcextra.cpp (More PC Options) */
int pcx_opt_find(const char* ini);
int pcx_opt_value(int i, char* out, int n);
void pcx_opt_set_value(int i, const char* v);
/* stubs.cpp: a PS3 button named for the device in use */
void r2_button_name(const char* pad, int mode, char* out, int n);
}

namespace {

enum { SRC_PCM, SRC_PCX, SRC_UPSCALE };
struct Choice { const char* shown; const char* value; };
enum { MAX_CH = 16 };
struct RowDef { const char* label; int src; const char* key; Choice ch[MAX_CH]; };

/* labels: short, upper case ('*' = takes effect at the next start). Values: exactly as the F10 menu /
 * launcher store them. */
/* the pages, grouped into the categories every port's options menu uses (R1 / R2 / R3) */
const RowDef k_display[] = {
    { "FRAME RATE *", SRC_PCM, "fps", { {"30 FPS", "30"}, {"40 FPS", "40"}, {"45 FPS", "45"}, {"60 FPS", "60"}, {"72 FPS", "72"},
                                        {"90 FPS", "90"}, {"120 FPS", "120"}, {"144 FPS", "144"}, {"165 FPS", "165"}, {"240 FPS", "240"},
                                        {"MATCH DISPLAY", "refresh"}, {"JUST UNDER REFRESH", "vrr"}, {"UNCAPPED", "0"} } },
    { "FIELD OF VIEW", SRC_PCM, "fov", { {"ORIGINAL", ""}, {"+5%", "0.95"}, {"+11%", "0.9"}, {"+18%", "0.85"}, {"+25%", "0.8"},
                                         {"+33%", "0.75"}, {"+43%", "0.7"}, {"+54%", "0.65"}, {"+67%", "0.6"} } },
    { "ASPECT RATIO *", SRC_PCM, "aspect", { {"16:9", ""}, {"FIT WINDOW", "auto"}, {"21:9", "21:9"}, {"32:9", "32:9"} } },
    { "BRIGHTNESS *", SRC_PCX, "gamma", { {"DEFAULT", ""}, {"BRIGHTER", "0.9"}, {"BRIGHTEST", "0.8"}, {"DARKER", "1.1"}, {"DARKEST", "1.2"} } },
    { "HDR OUTPUT *", SRC_PCX, "hdr", { {"OFF", ""}, {"AUTO", "auto"}, {"HDR10", "1"}, {"SCRGB", "scrgb"} } },
};
const RowDef k_graphics[] = {
    { "ANTI-ALIASING *", SRC_PCM, "fsr", { {"OFF", "0"}, {"AMD FSR 3.1", "1"}, {"NVIDIA DLSS", "2"}, {"INTEL XESS", "4"} } },
    { "UPSCALING *", SRC_UPSCALE, "upscale", { {"NATIVE", "1"}, {"QUALITY", "0.667"}, {"BALANCED", "0.58"}, {"PERFORMANCE", "0.5"} } },
    { "FRAME GENERATION *", SRC_PCX, "dlss_fg", { {"OFF", ""}, {"2X", "2"}, {"3X", "3"}, {"4X", "4"}, {"FSR 3 2X", "fsr3"} } },
    { "NVIDIA REFLEX *", SRC_PCX, "reflex", { {"OFF", ""}, {"ON", "1"}, {"ON + BOOST", "2"} } },
};
const RowDef k_effects[] = {
    { "SCREEN BLUR", SRC_PCX, "fx_blur", { {"ON", ""}, {"OFF", "0"} } },
    { "CAMERA SHAKE", SRC_PCX, "fx_shake", { {"ON", ""}, {"OFF", "0"} } },
    { "BLACK BARS", SRC_PCX, "fx_letterbox", { {"ON", ""}, {"OFF", "0"} } },
    { "COLOUR FILTER", SRC_PCX, "colorblind", { {"OFF", ""}, {"PROTANOPIA", "1"}, {"DEUTERANOPIA", "2"}, {"TRITANOPIA", "3"} } },
};
const RowDef k_controls1[] = {
    { "MOUSE SENSITIVITY", SRC_PCM, "mouse_sens", { {"0.25X", "0.25"}, {"0.5X", "0.5"}, {"0.75X", "0.75"}, {"1X", "1"}, {"1.25X", "1.25"},
                                                    {"1.5X", "1.5"}, {"2X", "2"}, {"2.5X", "2.5"}, {"3X", "3"} } },
    { "BUTTON PROMPTS", SRC_PCM, "padglyphs", { {"AUTO", ""}, {"KEYBOARD", "keys"}, {"XBOX", "xbox"}, {"PLAYSTATION", "ps"}, {"NINTENDO", "nintendo"} } },
    { "CROSSHAIR", SRC_PCX, "crosshair", { {"OFF", ""}, {"DOT", "dot"}, {"CROSS", "cross"} } },
    { "CROSSHAIR COLOUR", SRC_PCX, "xhair_color", { {"WHITE", ""}, {"GREEN", "green"}, {"YELLOW", "yellow"}, {"CYAN", "cyan"}, {"RED", "red"}, {"PINK", "pink"}, {"ORANGE", "orange"} } },
    { "CROSSHAIR SIZE", SRC_PCX, "xhair_size", { {"SMALL", ""}, {"MEDIUM", "medium"}, {"LARGE", "large"}, {"HUGE", "huge"} } },
    { "HOLD TO SKIP MOVIES", SRC_PCX, "cutscene_skip", { {"ON", ""}, {"OFF", "0"} } },
    { "SKIP UNWATCHED MOVIES", SRC_PCX, "cutscene_skip_all", { {"ON", ""}, {"OFF", "0"} } },
    { "SKIP INTRO LOGOS *", SRC_PCX, "skipintro", { {"ON", ""}, {"OFF", "0"} } },
    { "PAUSE ON PAD LOSS", SRC_PCX, "pad_disconnect_pause", { {"ON", ""}, {"OFF", "0"} } },
};
const RowDef k_controls2[] = {
    { "VIBRATION", SRC_PCX, "rumble", { {"100%", ""}, {"75%", "75"}, {"50%", "50"}, {"25%", "25"}, {"OFF", "0"} } },
    { "GYRO AIMING", SRC_PCX, "gyro", { {"OFF", ""}, {"ON", "1"}, {"WHILE AIMING", "aim"} } },
    { "GYRO SENSITIVITY", SRC_PCX, "gyrosens", { {"2X", ""}, {"0.5X", "0.5"}, {"1X", "1"}, {"1.5X", "1.5"}, {"3X", "3"}, {"4X", "4"}, {"6X", "6"} } },
    { "GYRO VERTICAL", SRC_PCX, "gyroinvy", { {"NORMAL", ""}, {"INVERTED", "1"} } },
    { "LIGHT BAR", SRC_PCX, "lightbar", { {"PLAYER COLOUR", ""}, {"OFF", "0"} } },
    { "NINTENDO LAYOUT", SRC_PCX, "nswap", { {"BY POSITION", ""}, {"NINTENDO", "1"} } },
};
const RowDef k_audio[] = {
    { "MASTER VOLUME", SRC_PCX, "volume", { {"100%", ""}, {"90%", "90"}, {"80%", "80"}, {"70%", "70"}, {"60%", "60"}, {"50%", "50"},
                                            {"40%", "40"}, {"30%", "30"}, {"20%", "20"}, {"10%", "10"}, {"MUTED", "0"} } },
    { "MONO AUDIO", SRC_PCX, "audio_mono", { {"OFF", ""}, {"ON", "1"} } },
    { "LANGUAGE *", SRC_PCX, "language", { {"WINDOWS", ""}, {"ENGLISH", "en"}, {"ENGLISH (UK)", "en-gb"}, {"FRENCH", "fr"},
                                           {"SPANISH", "es"}, {"GERMAN", "de"}, {"ITALIAN", "it"}, {"PORTUGUESE (BR)", "pt-br"},
                                           {"PORTUGUESE", "pt"}, {"DUTCH", "nl"}, {"RUSSIAN", "ru"}, {"POLISH", "pl"} } },
};
#define R2_ROWS(a) a, (int)(sizeof a / sizeof a[0])
enum { CAT_DISPLAY, CAT_GRAPHICS, CAT_EFFECTS, CAT_CONTROLS, CAT_AUDIO, NCATS };
const char* const kCatLabel[NCATS] = { "PC DISPLAY", "PC GRAPHICS", "PC EFFECTS", "PC CONTROLS & HUD", "PC AUDIO & LANGUAGE" };
struct Page { const RowDef* rows; int n; int cat; };
/* in category order: a category's pages are consecutive (r2swf::patch_avoptions checks it) */
const Page k_pages[] = {
    { R2_ROWS(k_display), CAT_DISPLAY },
    { R2_ROWS(k_graphics), CAT_GRAPHICS },
    { R2_ROWS(k_effects), CAT_EFFECTS },
    { R2_ROWS(k_controls1), CAT_CONTROLS },
    { R2_ROWS(k_controls2), CAT_CONTROLS },
    { R2_ROWS(k_audio), CAT_AUDIO },
};
#undef R2_ROWS
const int kNPages = (int)(sizeof k_pages / sizeof k_pages[0]);
// the game's options list has nine rows; r2swf::patch_avoptions rejects longer pages (no PC pages at all)
static_assert(sizeof k_display / sizeof k_display[0] <= 9 && sizeof k_graphics / sizeof k_graphics[0] <= 9 &&
              sizeof k_effects / sizeof k_effects[0] <= 9 && sizeof k_controls1 / sizeof k_controls1[0] <= 9 &&
              sizeof k_controls2 / sizeof k_controls2[0] <= 9 && sizeof k_audio / sizeof k_audio[0] <= 9,
              "a PC settings page has more than 9 rows");

int nch(const RowDef& r) { int n = 0; while (n < MAX_CH && r.ch[n].shown) n++; return n; }

std::mutex s_mu;

/* R2_MENU_ROWS: 0 / off = nothing; 2 = the pages behind L1 / R1 on Options > Audio/Visual only;
 * anything else (default 1) = also an Options > PC SETTINGS entry in the main menu */
int enabled_raw()
{
    const char* e = getenv("R2_MENU_ROWS");
    if (e && (*e == '0' || !_stricmp(e, "off"))) return 0;
    if (e && !strcmp(e, "2")) return 2;
    return 1;
}
int mode() { static const int s_mode = enabled_raw(); return s_mode; }

std::atomic<bool> s_entry{ false };     /* mainmenu.swf has the entry (r2_mods.cpp) */
std::atomic<bool> s_hook_live{ false }; /* r2_menu_rows_screen has seen the game's FSCommands */
std::atomic<bool> s_pc_open{ false };   /* the next AVOptions comes from the entry */
std::atomic<bool> s_pages{ false };      /* avoptions.swf has the PC pages (r2_mods.cpp) */
std::atomic<bool> s_pause_entry{ false }; /* pausemenu.swf has the entry (r2_mods.cpp) */

std::string current(const RowDef& r)
{
    char b[128] = "";
    switch (r.src) {
    case SRC_PCM: { const int i = pcm_opt_find(r.key); if (i >= 0) pcm_opt_value(i, b, sizeof b); break; }
    case SRC_PCX: { const int i = pcx_opt_find(r.key); if (i >= 0) pcx_opt_value(i, b, sizeof b); break; }
    case SRC_UPSCALE:
        if (!pcm_ini_get("upscale", b, sizeof b) || !*b) snprintf(b, sizeof b, "%s", "0.5");   /* the launcher's default */
        break;
    }
    return b;
}
bool numeric(const char* s, double& d)
{
    if (!s || !*s) return false;
    char* end = nullptr;
    d = strtod(s, &end);
    return end && !*end;
}
/* the choice index of value v: exact match, else the nearest number, else 0 */
int index_of(const RowDef& r, const std::string& v)
{
    const int n = nch(r);
    for (int k = 0; k < n; k++) if (!_stricmp(r.ch[k].value, v.c_str())) return k;
    double x, y;
    int best = -1; double bd = 1e300;
    if (numeric(v.c_str(), x))
        for (int k = 0; k < n; k++)
            if (numeric(r.ch[k].value, y) && fabs(x - y) < bd) { bd = fabs(x - y); best = k; }
    if (best >= 0) return best;
    fprintf(stderr, "[menurows] %s: value \"%s\" is not one of the page's choices, shown as %s\n", r.label, v.c_str(), r.ch[0].shown);
    return 0;
}
void apply(const RowDef& r, const char* v)
{
    switch (r.src) {
    case SRC_PCM: { const int i = pcm_opt_find(r.key); if (i >= 0) pcm_opt_set_value(i, v); break; }
    case SRC_PCX: { const int i = pcx_opt_find(r.key); if (i >= 0) pcx_opt_set_value(i, v); break; }
    case SRC_UPSCALE: {
        pcm_ini_set("upscale", v);
        char fsr[32] = "0";
        const int i = pcm_opt_find("fsr");
        if (i >= 0) pcm_opt_value(i, fsr, sizeof fsr);
        pcm_bat_set("RSX_UPSCALE", !strcmp(fsr, "0") ? "1" : v);   /* as launcher.cpp */
        break; }
    }
}

struct SavedRegs {
    uint64_t gpr[32]; double fpr[32]; ppu_vr vr[32];
    uint32_t cr, xer, fpscr, vscr; uint64_t lr, ctr;
};
void save_regs(const ppu_context* c, SavedRegs& s)
{
    memcpy(s.gpr, c->gpr, sizeof s.gpr); memcpy(s.fpr, c->fpr, sizeof s.fpr); memcpy(s.vr, c->vr, sizeof s.vr);
    s.cr = c->cr; s.xer = c->xer; s.fpscr = c->fpscr; s.vscr = c->vscr; s.lr = c->lr; s.ctr = c->ctr;
}
void load_regs(ppu_context* c, const SavedRegs& s)
{
    memcpy(c->gpr, s.gpr, sizeof s.gpr); memcpy(c->fpr, s.fpr, sizeof s.fpr); memcpy(c->vr, s.vr, sizeof s.vr);
    c->cr = s.cr; c->xer = s.xer; c->fpscr = s.fpscr; c->vscr = s.vscr; c->lr = s.lr; c->ctr = s.ctr;
}

bool guest_str_is(uint32_t ea, const char* s)
{
    if (!ea || ea >= 0xF0000000u) return false;
    return !memcmp(vm_base + ea, s, strlen(s) + 1);
}
/* the game's own "optionsArray_Values" string: the TOC entry func_002D23C4 itself passes to
 * SetVariableArray (r30 = [r2 - 0x7848], name = [r30 - 0x7E64]), else its known address */
uint32_t values_name(const ppu_context* ctx)
{
    static const char kName[] = "optionsArray_Values";
    const uint32_t r2 = (uint32_t)ctx->gpr[2];
    if (r2 && r2 < 0xF0000000u) {
        const uint32_t r30 = vm_read32(r2 - 0x7848);
        if (r30 && r30 < 0xF0000000u) {
            const uint32_t n = vm_read32(r30 - 0x7E64);
            if (guest_str_is(n, kName)) return n;
        }
    }
    return guest_str_is(0x00C10A58u, kName) ? 0x00C10A58u : 0;
}

/* _root.<name>[0..n-1] = numbers, through the game's SetVariableArray (GFxValue: u32 type at +0,
 * 3 = number, double at +8). name: a guest string, or nullptr to put `name_host` on our frame. */
void set_numbers(ppu_context* ctx, uint32_t movie, uint32_t name, const char* name_host, const double* v, int n)
{
    SavedRegs sv; save_regs(ctx, sv);
    const uint32_t r1 = (uint32_t)sv.gpr[1];
    const uint32_t frame = (0x200u + 16u * (uint32_t)n + 0xFFu) & ~0xFFu;
    const uint32_t sp = (r1 - frame - 0x200u) & ~0xFu;   /* our frame, below the caller's */
    vm_write64(sp, r1);                              /* back chain */
    if (!name && name_host) {
        name = sp + 0x80;
        memcpy(vm_base + name, name_host, strlen(name_host) + 1);   /* < 0x80 bytes */
    }
    const uint32_t vals = sp + 0x100;
    for (int r = 0; r < n; r++) {
        uint64_t b; memcpy(&b, &v[r], 8);
        vm_write32(vals + 16 * r, 3); vm_write32(vals + 16 * r + 4, 0); vm_write64(vals + 16 * r + 8, b);
    }
    ctx->gpr[1] = sp;
    ctx->gpr[3] = movie; ctx->gpr[4] = name; ctx->gpr[5] = 0; ctx->gpr[6] = vals; ctx->gpr[7] = (uint64_t)n; ctx->gpr[8] = 0;
    ctx->lr = sv.lr;
    func_00A93244(ctx);
    while (g_trampoline_fn) { void (*t)(void*) = g_trampoline_fn; g_trampoline_fn = 0; t((void*)ctx); }
    load_regs(ctx, sv);
}

/* "PcGet" <page>: _root.optionsArray_Values = [indices] */
void pc_get(ppu_context* ctx, int page)
{
    const Page& p = k_pages[page - 1];
    const uint32_t movie = (uint32_t)ctx->gpr[5], name = values_name(ctx);
    if (!movie || movie >= 0xF0000000u || !name) {
        fprintf(stderr, "[menurows] PcGet: no movie / name (movie 0x%08X), values left at the first choice\n", movie);
        return;
    }
    double idx[9] = {};
    for (int r = 0; r < p.n && r < 9; r++) idx[r] = (double)index_of(p.rows[r], current(p.rows[r]));
    set_numbers(ctx, movie, name, nullptr, idx, 9);
}

/* "PcHint": _root.pcHintC = the page keys as character codes and a 0, e.g. "RMB / LMB" -- L1 / R1
 * (previous / next page) named for the device in use (r2_button_name: the keys bound in
 * controls.txt, Xbox, PlayStation or Nintendo buttons). The movie asks on every key press, so the
 * subtitle follows the last used device. */
void pc_hint(ppu_context* ctx)
{
    const uint32_t movie = (uint32_t)ctx->gpr[5];
    if (!movie || movie >= 0xF0000000u) return;
    char l1[32], r1[32], text[72];
    r2_button_name("L1", -1, l1, sizeof l1);
    r2_button_name("R1", -1, r1, sizeof r1);
    snprintf(text, sizeof text, "%s / %s", l1, r1);
    double codes[72];
    int n = 0;
    for (const char* c = text; *c && n < 39; c++) codes[n++] = (double)(unsigned char)toupper((unsigned char)*c);
    codes[n++] = 0;
    set_numbers(ctx, movie, 0, "pcHintC", codes, n);
    static char s_last[72];
    if (strcmp(s_last, text)) { snprintf(s_last, sizeof s_last, "%s", text); fprintf(stderr, "[menurows] options screen: page keys %s\n", text); }
}

/* "PcStart" (the options screen opens): _root.pcStart = [opened from the main menu's PC SETTINGS
 * entry, that entry exists]. Without the entry the movie keeps L1 / R1 to the pages on the AV list. */
void pc_start(ppu_context* ctx)
{
    const uint32_t movie = (uint32_t)ctx->gpr[5];
    if (!movie || movie >= 0xF0000000u) return;
    const bool open = s_pc_open.exchange(false);
    const bool entry = mode() == 1 && s_entry.load() && s_hook_live.load();
    const double v[2] = { open ? 1.0 : 0.0, entry ? 1.0 : 0.0 };
    set_numbers(ctx, movie, 0, "pcStart", v, 2);
    fprintf(stderr, "[menurows] options screen: %s\n", open ? "opened from Options > PC SETTINGS, the PC categories"
                    : entry ? "Audio/Visual (the PC pages are under Options > PC SETTINGS)"
                    : "Audio/Visual, L1 / R1: the PC categories");
}

void pc_set(const char* args)
{
    int page = 0, row = 0, k = 0;
    if (sscanf(args, "%d:%d:%d", &page, &row, &k) != 3 || page < 1 || page > kNPages) {
        fprintf(stderr, "[menurows] PcSet \"%s\": not understood\n", args);
        return;
    }
    const Page& p = k_pages[page - 1];
    if (row < 0 || row >= p.n || k < 0 || k >= nch(p.rows[row])) {
        fprintf(stderr, "[menurows] PcSet \"%s\": out of range\n", args);
        return;
    }
    const RowDef& r = p.rows[row];
    if (index_of(r, current(r)) == k) return;
    apply(r, r.ch[k].value);
    fprintf(stderr, "[menurows] %s = %s (from the game's options screen)%s\n", r.label, r.ch[k].shown,
            strchr(r.label, '*') ? ", at the next start" : "");
}

bool guest_cstr(uint32_t ea, char* out, size_t n)
{
    if (!ea || ea >= 0xF0000000u || !n) return false;
    size_t i = 0;
    for (; i + 1 < n; i++) { out[i] = (char)vm_base[ea + i]; if (!out[i]) return true; }
    out[i] = 0;
    return true;
}

} // namespace

/* r2_mods.cpp */
bool r2_menu_rows_enabled() { return mode() != 0; }
int r2_menu_rows_mode() { return mode(); }
void r2_menu_rows_set_entry(bool on) { s_entry = on; }
void r2_menu_rows_set_pages(bool on) { s_pages = on; }
void r2_menu_rows_set_pause_entry(bool on) { s_pause_entry = on; }
bool r2_menu_rows_patch_mainmenu(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, std::string* why)
{
    return r2swf::patch_mainmenu(in, out, why);
}
/* the PC pages and their categories, as both movies show them */
static void build_pages(std::vector<std::vector<r2swf::PcRow>>& pages, std::vector<r2swf::PcCategory>& cats)
{
    pages.clear(); cats.clear();
    for (int p = 0; p < kNPages; p++) {
        std::vector<r2swf::PcRow> rows;
        for (int r = 0; r < k_pages[p].n; r++) {
            const RowDef& d = k_pages[p].rows[r];
            r2swf::PcRow row;
            row.label = d.label;
            for (int k = 0; k < nch(d); k++) row.choices.push_back(d.ch[k].shown);
            rows.push_back(std::move(row));
        }
        pages.push_back(std::move(rows));
    }
    for (int p = 0; p < kNPages; p++) {
        if (cats.size() == (size_t)k_pages[p].cat + 1) { cats.back().count++; continue; }
        cats.push_back({ kCatLabel[k_pages[p].cat], p, 1 });
    }
}
bool r2_menu_rows_patch_pausemenu(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, std::string* why)
{
    std::vector<std::vector<r2swf::PcRow>> pages;
    std::vector<r2swf::PcCategory> cats;
    build_pages(pages, cats);
    return r2swf::patch_pausemenu(in, pages, cats, out, why);
}
bool r2_menu_rows_patch_swf(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, std::string* why)
{
    std::vector<std::vector<r2swf::PcRow>> pages;
    std::vector<r2swf::PcCategory> cats;
    build_pages(pages, cats);
    return r2swf::patch_avoptions(in, pages, cats, out, why);
}

/* func_002D23C4 entry (options screen FSCommands). 1: ours, handled (the game's handler is skipped). */
extern "C" int r2_menu_rows_cmd(ppu_context* ctx)
{
    if (!vm_base) return 0;
    if (!mode()) return 0;
    const uint32_t cmd = (uint32_t)ctx->gpr[3];
    if (!cmd || cmd >= 0xF0000000u || vm_base[cmd] != 'P' || vm_base[cmd + 1] != 'c') return 0;
    const bool get = guest_str_is(cmd, "PcGet"), set = !get && guest_str_is(cmd, "PcSet");
    const bool hint = !get && !set && guest_str_is(cmd, "PcHint");
    const bool start = !get && !set && !hint && guest_str_is(cmd, "PcStart");
    if (!get && !set && !hint && !start) return 0;
    char args[64] = "";
    guest_cstr((uint32_t)ctx->gpr[4], args, sizeof args);
    std::lock_guard<std::mutex> lk(s_mu);
    if (hint) pc_hint(ctx);
    else if (start) pc_start(ctx);
    else if (get) {
        const int page = atoi(args);
        if (page >= 1 && page <= kNPages) {
            pc_get(ctx, page);
            static int s_logged = 0;
            if (s_logged < 3) { s_logged++; fprintf(stderr, "[menurows] options screen: PC page %d/%d shown (R2_MENU_ROWS=0 turns the PC pages off)\n", page, kNPages); }
        }
    } else pc_set(args);
    ctx->gpr[3] = 0;
    return 1;
}

/* func_00769378 entry: the game's generic FSCommand processor (SetScreen, PushScreen, Localize2,
 * PlaySound, ... ; cmd r4, args r5), the only code that knows "SetScreen". The main menu's
 * Options > PC SETTINGS entry sends SetScreen "PcSettings": opened as AVOptions, and its "PcStart"
 * then answers "from the entry" (the PC category list). Never skips the game's code. */
extern "C" void r2_menu_rows_screen(ppu_context* ctx)
{
    if (!vm_base || mode() != 1) return;
    if (!s_hook_live.load(std::memory_order_relaxed)) s_hook_live = true;
    const uint32_t cmd = (uint32_t)ctx->gpr[4];
    if (!cmd || cmd >= 0xF0000000u - 16) return;
    const uint32_t args = (uint32_t)ctx->gpr[5];
    if (vm_base[cmd] != 'S' || vm_base[cmd + 3] != 'S' || !guest_str_is(cmd, "SetScreen")) return;
    if (!guest_str_is(args, "PcSettings")) {
        if (s_pc_open.load(std::memory_order_relaxed)) s_pc_open = false;
        return;
    }
    static const uint32_t kAVOptions = 0x00C0BD38u;   /* the game's own "AVOptions" (its main menu entry's argument) */
    if (guest_str_is(kAVOptions, "AVOptions")) ctx->gpr[5] = kAVOptions;
    else memcpy(vm_base + args, "AVOptions", 10);    /* in place: "PcSettings" is longer */
    s_pc_open = true;
    static int s_logged = 0;
    if (s_logged < 3) { s_logged++; fprintf(stderr, "[menurows] main menu: Options > PC SETTINGS -> the options screen on the PC pages\n"); }
}
