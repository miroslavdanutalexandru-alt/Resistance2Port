/* Resistance 2 port -- Holar Tower elevator assist (R2_HOLAR_ASSIST, default 1).
 *
 * In "Holar Tower" (iceland2) the lobby elevator leaves only when Warner, Capelli,
 * Hawthorne and Hale all stand on the platform (iceland2_seg7.lua: is_inside(..,
 * S07_TRIG_EPLATUP_ALLON2/4) -> "ALL SENTINELS AND HALE ON ELEVATOR PLATFORM").
 * iceland2_seg7_b.lua orders the three sentinels there exactly once (hold_position,
 * one frame after innate_behavior) when the last cocoon pops. If that one-shot order
 * is lost (AI combat/pathing), the crew stands in the lobby forever and the
 * checkpoint never comes (RPCS3 #11590 / #5831, "crew never move").
 *
 * The fix adds a watchdog to the loop of that script at load time:
 *   20 s after the order: give the script's own orders again;
 *   45 s, once Hale has stood on the platform for 6 s: set_position + hold_position
 *        the sentinels that are not on it onto their hold_position spots;
 *   once Hale has stood on the platform for another 10 s: run the gate's own body
 *        (esents5=true, idle, close the lobby door, move the elevator).
 * Each step is skipped once the original gate has fired (esents5 ~= nil).
 *
 * The text is patched in guest memory where Lua's luaZ_fill (func_007404F8) gets
 * a chunk from its reader (hook in tools/hooks/r2.json), only when the chunk is
 * byte for byte the retail iceland2_seg7_b.lua (size + FNV-1a). Comment-only and
 * blank lines and indentation are dropped to make room, so the chunk keeps its
 * size (padded with newlines); nothing is
 * read from or written to disk. Messages go through prt() and show in run.log as
 * [holar-assist] lines. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

extern "C" uint8_t* vm_base;

static const uint32_t kSeg7bSize = 4611;
static const uint64_t kSeg7bFnv = 0xeaa1494944b7d2a6ull;   /* FNV-1a 64 of the retail file */

static const char kWatchdog[] =
"if (mstrtime4==true and esents5==nil) then\n"
"hlra_t=(hlra_t or 0)+(step_time or 0.033)\n"
"if is_inside(HALE, S07_TRIG_EPLATUP_ALLON2) then hlra_h=(hlra_h or 0)+(step_time or 0.033) else hlra_h=0 end\n"
"if (hlra_s==nil and hlra_t>20) then\n"
"hlra_s=1\n"
"prt(\"holar-assist: crew not on the elevator 20 s after the order, giving it again\")\n"
"clear_home_area(SENTINELS)\n"
"wait()\n"
"innate_behavior(SENTINELS)\n"
"wait()\n"
"if (esents5==nil) then\n"
"hold_position(WARNER, S07_HOPO_EPLATUP_1)\n"
"hold_position(CAPELLI, S07_HOPO_EPLATUP_2)\n"
"hold_position(HAWTHORNE, S07_HOPO_EPLATUP_3)\n"
"end\n"
"elseif (hlra_s==1 and hlra_t>45 and hlra_h>6) then\n"
"hlra_s=2 hlra_h=0\n"
"prt(\"holar-assist: Hale on the platform, moving the crew that is not\")\n"
"if not is_inside(WARNER, S07_TRIG_EPLATUP_ALLON4) then prt(\"holar-assist: Warner\") set_position(WARNER, S07_HOPO_EPLATUP_1) hold_position(WARNER, S07_HOPO_EPLATUP_1) end\n"
"if not is_inside(CAPELLI, S07_TRIG_EPLATUP_ALLON2) then prt(\"holar-assist: Capelli\") set_position(CAPELLI, S07_HOPO_EPLATUP_2) hold_position(CAPELLI, S07_HOPO_EPLATUP_2) end\n"
"if not is_inside(HAWTHORNE, S07_TRIG_EPLATUP_ALLON2) then prt(\"holar-assist: Hawthorne\") set_position(HAWTHORNE, S07_HOPO_EPLATUP_3) hold_position(HAWTHORNE, S07_HOPO_EPLATUP_3) end\n"
"elseif (hlra_s==2 and hlra_h>10) then\n"
"hlra_s=3 esents5=true\n"
"prt(\"holar-assist: platform check still failing, starting the elevator\")\n"
"prt(\"ALL SENTINELS AND HALE ON ELEVATOR PLATFORM\")\n"
"idle(SENTINELS)\n"
"wait()\n"
"close_door(ELEVATOR_LOBBY_SHAFT_DOOR)\n"
"move_elevator(S07_ELEVATOR_PLATFORM1)\n"
"wait(3)\n"
"lztimev=false lsztimev=false rztimev=false rsztimev=false lticz0=false rticz0=false\n"
"end\n"
"elseif (mstrtime4~=true) then\n"
"hlra_t=nil hlra_h=nil hlra_s=nil\n"
"end\n";

static uint64_t fnv1a(const uint8_t* p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}

/* retail text -> same-size text with the watchdog; false if it does not fit/match */
static bool holar_transform(const uint8_t* in, size_t n, std::string& out)
{
    std::string txt; txt.reserve(n);
    size_t i = 0;
    while (i < n) {
        size_t e = i; while (e < n && in[e] != '\n') e++;
        size_t le = e; if (le > i && in[le - 1] == '\r') le--;
        size_t s = i; while (s < le && (in[s] == ' ' || in[s] == '\t')) s++;
        if (s < le && !(le - s >= 2 && in[s] == '-' && in[s + 1] == '-')) { txt.append((const char*)in + s, le - s); txt += '\n'; }
        i = e + 1;
    }
    static const char anchor[] = "\nwait()\nend\n";
    const size_t al = sizeof anchor - 1;
    if (txt.size() < al || txt.compare(txt.size() - al, al, anchor) != 0) return false;
    out = txt.substr(0, txt.size() - al + 1) + kWatchdog + (anchor + 1);
    if (out.size() > n) return false;
    out.append(n - out.size(), '\n');
    return true;
}

static int holar_mode()
{
    static int m = -1;
    if (m < 0) { const char* e = getenv("R2_HOLAR_ASSIST"); m = (e && (*e == '0' || !_stricmp(e, "off"))) ? 0 : 1; }
    return m;
}

/* luaZ_fill, right after the reader returned buf (guest) and *size_ea (guest u32) */
extern "C" void r2x_lua_chunk(uint64_t buf, uint64_t size_ea)
{
    if (!vm_base) return;
    const uint32_t b = (uint32_t)buf, se = (uint32_t)size_ea;
    if (b < 0x10000u || se < 0x10000u) return;
    const uint8_t* sp = vm_base + se;
    const uint32_t n = ((uint32_t)sp[0] << 24) | ((uint32_t)sp[1] << 16) | ((uint32_t)sp[2] << 8) | sp[3];
    static const char head[] = "---------------\r\n--|SEGMENT 7|--\r\n---------------\r\n\r\n--THE FOLLOWING SCRIPT HELPS SUPPORT";
    if (n < sizeof head - 1 || (uint64_t)b + n > 0xE0000000ull) return;
    uint8_t* p = vm_base + b;
    if (memcmp(p, head, sizeof head - 1) != 0) return;
    if (n != kSeg7bSize || fnv1a(p, n) != kSeg7bFnv) {
        fprintf(stderr, "[holar-assist] iceland2_seg7_b.lua chunk (%u bytes) differs from the retail script - not patched\n", n);
        return;
    }
    if (!holar_mode()) { fprintf(stderr, "[holar-assist] R2_HOLAR_ASSIST=0: elevator watchdog off\n"); return; }
    std::string out;
    if (!holar_transform(p, n, out)) { fprintf(stderr, "[holar-assist] could not fit the watchdog - not patched\n"); return; }
    memcpy(p, out.data(), n);
    fprintf(stderr, "[holar-assist] iceland2_seg7_b.lua: elevator watchdog added (R2_HOLAR_ASSIST=0 turns it off)\n");
}

/* screen_out/prt lines from the watchdog: true = handled */
extern "C" int r2x_holar_prt(const char* msg)
{
    static const char tag[] = "holar-assist: ";
    if (strncmp(msg, tag, sizeof tag - 1) != 0) return 0;
    fprintf(stderr, "[holar-assist] %s\n", msg + sizeof tag - 1);
    return 1;
}
