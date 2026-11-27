/* Resistance 2 port -- Holar Tower elevator assist (R2_HOLAR_ASSIST, default 1).
 *
 * Two elevators in "Holar Tower" (iceland2) leave only when Hale and the three
 * sentinels (Warner, Capelli, Hawthorne) all stand in a trigger volume, and the level
 * script orders the sentinels there once. If that one-shot order is lost (AI
 * combat/pathing), the crew stands still and the elevator never moves (RPCS3 #11590 /
 * #5831, "crew never move"):
 *   iceland2_seg7_b.lua  lobby elevator (S07_TRIG_EPLATUP_ALLON2/4, gate esents5);
 *   iceland2_seg9.lua    node shaft elevator after the first Titan of the battlefield
 *                        (S09_TRIG_SENTS_NDSHAFT_1, gate bfe3: a sentinel that was not
 *                        in the node base when the Titan died is sent to
 *                        S09_TITN_GOTO_4 and only re-routed to the shaft if it walks
 *                        through S08F_TRIG_NODEBASE_7), and the ride back down
 *                        (S10_TRIG_SENTS_1..3, gate bfe7).
 *
 * A watchdog is added to the main loop of each script at load time:
 *   every 15 s (from 15-20 s after the order): each sentinel not in place gets the
 *        script's own order again (clear_home_area, innate_behavior, next frame
 *        hold_position on its spot);
 *   after 60 s, once Hale has stood in the volume for 6 s: set_position +
 *        hold_position the sentinels still missing (no more repeats after this);
 *   once Hale has stood there for another 10 s: run the gate's own body.
 * seg9 also moves on if the Titan has been dead for 2 min but the hybrid count check
 * still fails, if the path leaper count check still fails 5 min after the second
 * Titan, or if the "are we clear" dialogue callback never comes. Each step is skipped
 * once the original gate has fired. The watchdog never calls wait().
 *
 * The game loads either the text script or its compiled form (built/...lc, Lua 5.1
 * bytecode: big-endian, 4-byte float numbers, stripped). The chunk is replaced where
 * Lua's luaZ_fill (func_007404F8) gets it from its reader (hook in tools/hooks/r2.json),
 * only when it is byte for byte the retail file (size + FNV-1a):
 *   text:     the watchdog text is put before the loop's last "wait() end";
 *   bytecode: the watchdog, compiled ahead of time by tools/r2_holar_lc.py
 *             (project/r2_holar_lc.h, the port's own code only), is added as a nested
 *             function; the loop's back jump goes to instructions added after the
 *             final RETURN (CLOSURE, CALL, JMP back to the loop start, RETURN). Nothing
 *             else moves.
 * The longer result is handed to Lua through the reader's own buffer, one buffer-full
 * per luaZ_fill call (the reader is one-shot: its later calls return nothing). Nothing
 * is read from or written to disk. Messages go through prt() and show in run.log as
 * [holar-assist] lines. */
#include <stdint.h>
#include <stdio.h>
#include <algorithm>
#include <stdlib.h>
#include <string.h>
#include <mutex>
#include <string>
#include <vector>

extern "C" uint8_t* vm_base;

/* ---- watchdog Lua (also compiled by tools/r2_holar_lc.py: keep each one a single
 * string literal made of "..." lines) ---- */
static const char kWatchdog7[] =
"local hlra_dt=step_time or 0.033\n"
"if (hlra_q~=nil) then\n"
"local q=hlra_q hlra_q=nil\n"
"for i=1,#q do if (esents5==nil) then hold_position(q[i][1], q[i][2]) end end\n"
"end\n"
"hlra_m=hlra_m or function(w, v, p, n, f)\n"
"if (esents5==nil and not is_inside(w, v)) then\n"
"if f then prt(\"holar-assist: moving \"..n..\" onto the platform\") set_position(w, p) hold_position(w, p)\n"
"else prt(\"holar-assist: \"..n..\" not on the platform, giving the order again\") clear_home_area(w) innate_behavior(w) hlra_q=hlra_q or {} hlra_q[#hlra_q+1]={w, p} end\n"
"end\n"
"end\n"
"if (hlra_s==2) then\n"
"hlra_s=3 hlra_w=0\n"
"close_door(ELEVATOR_LOBBY_SHAFT_DOOR)\n"
"move_elevator(S07_ELEVATOR_PLATFORM1)\n"
"elseif (hlra_s==3) then\n"
"hlra_w=hlra_w+hlra_dt\n"
"if (hlra_w>=3) then\n"
"hlra_s=4\n"
"lztimev=false lsztimev=false rztimev=false rsztimev=false lticz0=false rticz0=false\n"
"end\n"
"elseif (mstrtime4==true and esents5==nil) then\n"
"hlra_t=(hlra_t or 0)+hlra_dt\n"
"if is_inside(HALE, S07_TRIG_EPLATUP_ALLON2) then hlra_h=(hlra_h or 0)+hlra_dt else hlra_h=0 end\n"
"if (hlra_s==nil and hlra_t>60 and hlra_h>6) then\n"
"hlra_s=1 hlra_h=0\n"
"hlra_m(WARNER, S07_TRIG_EPLATUP_ALLON4, S07_HOPO_EPLATUP_1, \"Warner\", true)\n"
"hlra_m(CAPELLI, S07_TRIG_EPLATUP_ALLON2, S07_HOPO_EPLATUP_2, \"Capelli\", true)\n"
"hlra_m(HAWTHORNE, S07_TRIG_EPLATUP_ALLON2, S07_HOPO_EPLATUP_3, \"Hawthorne\", true)\n"
"elseif (hlra_s==nil and hlra_t>=(hlra_n or 15)) then\n"
"hlra_n=(hlra_n or 15)+15\n"
"hlra_m(WARNER, S07_TRIG_EPLATUP_ALLON4, S07_HOPO_EPLATUP_1, \"Warner\")\n"
"hlra_m(CAPELLI, S07_TRIG_EPLATUP_ALLON2, S07_HOPO_EPLATUP_2, \"Capelli\")\n"
"hlra_m(HAWTHORNE, S07_TRIG_EPLATUP_ALLON2, S07_HOPO_EPLATUP_3, \"Hawthorne\")\n"
"elseif (hlra_s==1 and hlra_h>10) then\n"
"hlra_s=2 esents5=true\n"
"prt(\"holar-assist: platform check still failing, starting the elevator\")\n"
"prt(\"ALL SENTINELS AND HALE ON ELEVATOR PLATFORM\")\n"
"idle(SENTINELS)\n"
"end\n"
"elseif (mstrtime4~=true) then\n"
"hlra_t=nil hlra_h=nil hlra_s=nil hlra_n=nil\n"
"end\n";

static const char kWatchdog9[] =
"local h9_dt=step_time or 0.033\n"
"if (h9_q~=nil) then\n"
"local q=h9_q h9_q=nil\n"
"for i=1,#q do if q[i][3]() then hold_position(q[i][1], q[i][2]) end end\n"
"end\n"
"h9_m=h9_m or function(w, v, p, n, f, g, where)\n"
"if (g() and not is_inside(w, v)) then\n"
"if f then prt(\"holar-assist: moving \"..n..\" to the \"..where) set_position(w, p) hold_position(w, p)\n"
"else prt(\"holar-assist: \"..n..\" not at the \"..where..\", giving the order again\") clear_home_area(w) innate_behavior(w) h9_q=h9_q or {} h9_q[#h9_q+1]={w, p, g} end\n"
"end\n"
"end\n"
"h9_g3=h9_g3 or function() return bfe3==nil end\n"
"h9_g7=h9_g7 or function() return bfe7==nil end\n"
"if (bfe0b==nil and bfe0a==false and is_dead(S08A_TITN_ENTRY_2)) then\n"
"h9_k=(h9_k or 0)+h9_dt\n"
"if (h9_k>120) then\n"
"prt(\"holar-assist: Titan dead for 2 min but the hybrid check still fails, moving on\")\n"
"bfe0b=true\n"
"prt(\"Titan Conquered\")\n"
"add_on_screen_objective(S08_SETPO_NDSHAFT_HAWTHORNE_1)\n"
"clear_home_area(SENTINELS)\n"
"innate_behavior(SENTINELS)\n"
"cpbf3=true\n"
"end\n"
"end\n"
"if (bfe3==nil and (cpbf3==true or cpbf3load==true)) then\n"
"h9_t=(h9_t or 0)+h9_dt\n"
"if is_inside(HALE, S09_TRIG_SENTS_NDSHAFT_1) then h9_h=(h9_h or 0)+h9_dt else h9_h=0 end\n"
"if (h9_s==nil and h9_t>60 and h9_h>6) then\n"
"h9_s=1 h9_h=0\n"
"h9_m(WARNER, S09_TRIG_SENTS_NDSHAFT_1, S08_SETPO_NDSHAFT_WARNER_1, \"Warner\", true, h9_g3, \"shaft elevator\")\n"
"h9_m(HAWTHORNE, S09_TRIG_SENTS_NDSHAFT_1, S08_SETPO_NDSHAFT_HAWTHORNE_1, \"Hawthorne\", true, h9_g3, \"shaft elevator\")\n"
"h9_m(CAPELLI, S09_TRIG_SENTS_NDSHAFT_1, S08_SETPO_NDSHAFT_CAPELLI_1, \"Capelli\", true, h9_g3, \"shaft elevator\")\n"
"elseif (h9_s==nil and h9_t>=(h9_n or 20)) then\n"
"h9_n=(h9_n or 20)+15\n"
"h9_m(WARNER, S09_TRIG_SENTS_NDSHAFT_1, S08_SETPO_NDSHAFT_WARNER_1, \"Warner\", false, h9_g3, \"shaft elevator\")\n"
"h9_m(HAWTHORNE, S09_TRIG_SENTS_NDSHAFT_1, S08_SETPO_NDSHAFT_HAWTHORNE_1, \"Hawthorne\", false, h9_g3, \"shaft elevator\")\n"
"h9_m(CAPELLI, S09_TRIG_SENTS_NDSHAFT_1, S08_SETPO_NDSHAFT_CAPELLI_1, \"Capelli\", false, h9_g3, \"shaft elevator\")\n"
"elseif (h9_s==1 and h9_h>10) then\n"
"h9_s=2\n"
"prt(\"holar-assist: shaft elevator check still failing, starting the elevator\")\n"
"bfe3=true\n"
"ndshft0=true\n"
"set_setup_name(\"Elevation\")\n"
"prt(\"elevator started\")\n"
"remove_on_screen_objective(S08_SETPO_NDSHAFT_HAWTHORNE_1)\n"
"show(S09_POD_SWRMR_PODVATOR_1)\n"
"show(S08B_TITN_NODEVATOR_0)\n"
"elevatoremit=play_explicit_sound_spec(SoundSpecs.amb_iceland2_swarmerElevator)\n"
"move_elevator(S10A_NODE_ELEVATOR_BOTTOM)\n"
"end\n"
"end\n"
"if (bfe5b==true and bfe7==nil) then\n"
"h9_u=(h9_u or 0)+h9_dt\n"
"if (bfe5c==nil and h9_u>300) then\n"
"prt(\"holar-assist: leaper count check still failing 5 min after the Titan, moving on\")\n"
"bfe5c=true\n"
"leapercounter=false\n"
"prt(\"path leapers eliminated\")\n"
"killswarmconv=true\n"
"end\n"
"if (bfe5c==true and enodedown==nil) then\n"
"h9_c=(h9_c or 0)+h9_dt\n"
"if (h9_c>45) then\n"
"prt(\"holar-assist: no order to go down after the dialogue, giving it\")\n"
"enodedown=true\n"
"end\n"
"end\n"
"if (enodedown~=nil) then\n"
"h9_v=(h9_v or 0)+h9_dt\n"
"if is_inside(HALE, S09_TRIG_ELEVATOR_DOWN_1) then h9_i=(h9_i or 0)+h9_dt else h9_i=0 end\n"
"if (h9_r==nil and h9_v>60 and h9_i>6) then\n"
"h9_r=1 h9_i=0\n"
"h9_m(WARNER, S10_TRIG_SENTS_1, S10_HOPO_SENTS_1, \"Warner\", true, h9_g7, \"elevator top\")\n"
"h9_m(HAWTHORNE, S10_TRIG_SENTS_2, S10_HOPO_SENTS_2, \"Hawthorne\", true, h9_g7, \"elevator top\")\n"
"h9_m(CAPELLI, S10_TRIG_SENTS_3, S10_HOPO_SENTS_3, \"Capelli\", true, h9_g7, \"elevator top\")\n"
"elseif (h9_r==nil and h9_v>=(h9_x or 20)) then\n"
"h9_x=(h9_x or 20)+15\n"
"h9_m(WARNER, S10_TRIG_SENTS_1, S10_HOPO_SENTS_1, \"Warner\", false, h9_g7, \"elevator top\")\n"
"h9_m(HAWTHORNE, S10_TRIG_SENTS_2, S10_HOPO_SENTS_2, \"Hawthorne\", false, h9_g7, \"elevator top\")\n"
"h9_m(CAPELLI, S10_TRIG_SENTS_3, S10_HOPO_SENTS_3, \"Capelli\", false, h9_g7, \"elevator top\")\n"
"elseif (h9_r==1 and h9_i>10) then\n"
"h9_r=2\n"
"prt(\"holar-assist: elevator top check still failing, going down\")\n"
"bfe7=true\n"
"prt(\"all sentinels present and accounted, going down\")\n"
"move_elevator(S10A_NODE_ELEVATOR_BOTTOM)\n"
"remove_on_screen_objective(S10A_NODE_ELEVATOR_TOP)\n"
"clear_home_area(SENTINELS)\n"
"h9_d=0\n"
"end\n"
"end\n"
"end\n"
"if (h9_d~=nil) then\n"
"h9_d=h9_d+h9_dt\n"
"if (h9_d>=5) then h9_d=nil ndshft2=true end\n"
"end\n";

#include "r2_holar_lc.h"   /* kWd7Code/kWd9Code + source FNVs: tools/r2_holar_lc.py */

static uint64_t fnv1a(const uint8_t* p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}

/* retail text -> text with the watchdog before the main loop's last "wait() end" */
static bool holar_text(const uint8_t* in, size_t n, const char* wd, std::string& out)
{
    size_t e = n;
    while (e > 0 && (in[e - 1] == ' ' || in[e - 1] == '\t' || in[e - 1] == '\r' || in[e - 1] == '\n')) e--;
    if (e < 3 || memcmp(in + e - 3, "end", 3) != 0) return false;
    size_t w = e - 3;
    while (w > 0 && (in[w - 1] == ' ' || in[w - 1] == '\t' || in[w - 1] == '\r' || in[w - 1] == '\n')) w--;
    if (w < 6 || memcmp(in + w - 6, "wait()", 6) != 0) return false;
    w -= 6;
    out.assign((const char*)in, w);
    out += "\ndo\n"; out += wd; out += "end\n";
    out.append((const char*)in + w, n - w);
    return true;
}

static uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void put32(std::string& s, uint32_t v) { s += (char)(v >> 24); s += (char)(v >> 16); s += (char)(v >> 8); s += (char)v; }

/* retail stripped bytecode (main function, no nested functions) -> + watchdog function,
 * main loop's back jump routed through CLOSURE/CALL of it */
static bool holar_bytecode(const uint8_t* in, size_t n, const uint8_t* wd, size_t wdn, std::string& out)
{
    static const uint8_t hdr[12] = { 0x1b, 'L', 'u', 'a', 0x51, 0, 0, 4, 4, 4, 4, 0 };
    if (n < 12 + 24 || memcmp(in, hdr, 12) != 0) return false;
    size_t o = 12;
    const uint32_t srcn = be32(in + o); o += 4 + srcn;
    o += 8;                                        /* linedefined, lastlinedefined */
    if (o + 4 > n) return false;
    const size_t o_ms = o + 3;
    const uint8_t ms = in[o_ms];
    o += 4;
    if (o + 4 > n) return false;
    const uint32_t ncode = be32(in + o); const size_t o_ncode = o; o += 4;
    if (ncode < 5 || o + 4ull * ncode > n) return false;
    const size_t o_code = o; o += 4ull * ncode;
    const size_t o_k = o;
    if (o + 4 > n) return false;
    uint32_t nk = be32(in + o); o += 4;
    std::vector<size_t> kstr(nk, 0);
    for (uint32_t i = 0; i < nk; i++) {
        if (o >= n) return false;
        const uint8_t t = in[o++];
        if (t == 0) {}
        else if (t == 1) o += 1;
        else if (t == 3) o += 4;
        else if (t == 4) { if (o + 4 > n) return false; kstr[i] = o; o += 4 + be32(in + o); }
        else return false;
    }
    const size_t o_p = o;
    if (o + 16 != n) return false;                 /* sizep, lineinfo, locvars, upvalues */
    for (int i = 0; i < 4; i++) if (be32(in + o + 4 * i) != 0) return false;
    auto ins = [&](uint32_t i) { return be32(in + o_code + 4ull * i); };
    const uint32_t iret = ins(ncode - 1), ijmp = ins(ncode - 2), icall = ins(ncode - 3), iget = ins(ncode - 4);
    if ((iret & 63) != 30 || (ijmp & 63) != 22 || (icall & 63) != 28 || (iget & 63) != 5) return false;
    const int sbx = (int)(ijmp >> 14) - 131071;
    const int loop = (int)(ncode - 2) + 1 + sbx;
    if (sbx >= 0 || loop < 0) return false;
    const uint32_t ga = (iget >> 6) & 255, gk = iget >> 14;
    if (((icall >> 6) & 255) != ga || (icall >> 23) != 1 || ((icall >> 14) & 511) != 1) return false;
    if (gk >= nk || !kstr[gk] || be32(in + kstr[gk]) != 5 || memcmp(in + kstr[gk] + 4, "wait", 5) != 0) return false;
    if (ms > 247) return false;
    const uint32_t r = ms;
    out.assign((const char*)in, o_ms);
    out += (char)(ms + 2);
    out.append((const char*)in + o_ms + 1, o_ncode - (o_ms + 1));
    put32(out, ncode + 4);
    out.append((const char*)in + o_code, 4ull * (ncode - 2));
    put32(out, 22u | ((uint32_t)(1 + 131071) << 14));                       /* JMP +1 -> added block */
    out.append((const char*)in + o_code + 4ull * (ncode - 1), 4);           /* RETURN 0 1 */
    put32(out, 36u | (r << 6));                                             /* CLOSURE r, P[0] */
    put32(out, 28u | (r << 6) | (1u << 14) | (1u << 23));                   /* CALL r 1 1 */
    put32(out, 22u | ((uint32_t)(loop - (int)(ncode + 2) - 1 + 131071) << 14)); /* JMP loop start */
    out.append((const char*)in + o_code + 4ull * (ncode - 1), 4);           /* RETURN 0 1 (never reached; the verifier wants it last) */
    out.append((const char*)in + o_k, o_p - o_k);
    put32(out, 1);
    out.append((const char*)wd, wdn);
    put32(out, 0); put32(out, 0); put32(out, 0);
    return true;
}

static int holar_mode()
{
    static int m = -1;
    if (m < 0) { const char* e = getenv("R2_HOLAR_ASSIST"); m = (e && (*e == '0' || !_stricmp(e, "off"))) ? 0 : 1; }
    return m;
}

struct HolarScript {
    const char* name; uint32_t size; uint64_t fnv; bool bytecode;
    const char* wd; const uint8_t* code; size_t coden; uint64_t wdfnv;
};
static const HolarScript kScripts[] = {
    { "iceland2_seg7_b.lua", 4611,  0xeaa1494944b7d2a6ull, false, kWatchdog7, nullptr, 0, 0 },
    { "iceland2_seg7_b.lc",  3510,  0xe87092cfcba1f28bull, true,  kWatchdog7, kWd7Code, sizeof kWd7Code, kWd7SrcFnv },
    { "iceland2_seg9.lua",   17041, 0x9399f51c5e892206ull, false, kWatchdog9, nullptr, 0, 0 },
    { "iceland2_seg9.lc",    10792, 0x572c050bc7f4fe91ull, true,  kWatchdog9, kWd9Code, sizeof kWd9Code, kWd9SrcFnv },
};

/* a patched chunk still being handed out through the reader's buffer */
struct HolarStream { uint32_t zio, buf, cap; std::string data; size_t off; };
static std::mutex s_mu;
static std::vector<HolarStream> s_streams;

static void put_size(uint32_t se, uint32_t v) { uint8_t* sp = vm_base + se; sp[0] = (uint8_t)(v >> 24); sp[1] = (uint8_t)(v >> 16); sp[2] = (uint8_t)(v >> 8); sp[3] = (uint8_t)v; }

/* luaZ_fill, right after the reader returned buf (guest) and *size_ea (guest u32);
 * returns the buffer luaZ_fill goes on with (buf unless a patched chunk is streamed) */
extern "C" uint64_t r2x_lua_chunk(uint64_t zio, uint64_t buf, uint64_t size_ea)
{
    if (!vm_base) return buf;
    const uint32_t z = (uint32_t)zio, b = (uint32_t)buf, se = (uint32_t)size_ea;
    if (se < 0x10000u) return buf;
    const uint8_t* sp = vm_base + se;
    const uint32_t n = be32(sp);
    {
        std::lock_guard<std::mutex> lk(s_mu);
        for (size_t i = 0; i < s_streams.size(); i++) {
            HolarStream& s = s_streams[i];
            if (s.zio != z) continue;
            if (b != 0 && n != 0) {          /* the reader had more after all: give up on this one */
                fprintf(stderr, "[holar-assist] reader returned more data for a patched script - stream dropped\n");
                s_streams.erase(s_streams.begin() + i);
                return buf;
            }
            const uint32_t k = (uint32_t)std::min<size_t>(s.cap, s.data.size() - s.off);
            const uint32_t sb = s.buf;
            memcpy(vm_base + sb, s.data.data() + s.off, k);
            s.off += k;
            if (s.off >= s.data.size()) s_streams.erase(s_streams.begin() + i);
            put_size(se, k);
            return sb;
        }
    }
    if (b < 0x10000u || n < 16 || (uint64_t)b + n > 0xE0000000ull) return buf;
    const uint8_t* p = vm_base + b;
    const bool lc = p[0] == 0x1b && p[1] == 'L' && p[2] == 'u' && p[3] == 'a';
    static const char head7[] = "---------------\r\n--|SEGMENT 7|--\r\n---------------\r\n\r\n--THE FOLLOWING SCRIPT HELPS SUPPORT";
    static const char head9[] = "---------------\r\n--|SEGMENT 9|--\r\n---------------\r\n\r\n--The following setups occur at the end";
    const HolarScript* hs = nullptr;
    if (lc) {
        if (n != 3510 && n != 10792) return buf;
        const uint64_t h = fnv1a(p, n);
        for (const HolarScript& s : kScripts) if (s.bytecode && s.size == n && s.fnv == h) hs = &s;
        if (!hs) return buf;
    } else {
        const bool is7 = n >= sizeof head7 - 1 && memcmp(p, head7, sizeof head7 - 1) == 0;
        const bool is9 = n >= sizeof head9 - 1 && memcmp(p, head9, sizeof head9 - 1) == 0;
        if (!is7 && !is9) return buf;
        const uint64_t h = fnv1a(p, n);
        for (const HolarScript& s : kScripts) if (!s.bytecode && s.size == n && s.fnv == h) hs = &s;
        if (!hs) {
            fprintf(stderr, "[holar-assist] %s chunk (%u bytes) differs from the retail script - not patched\n", is7 ? "iceland2_seg7_b.lua" : "iceland2_seg9.lua", n);
            return buf;
        }
    }
    if (!holar_mode()) { fprintf(stderr, "[holar-assist] R2_HOLAR_ASSIST=0: %s left as is\n", hs->name); return buf; }
    std::string out;
    if (hs->bytecode) {
        if (fnv1a((const uint8_t*)hs->wd, strlen(hs->wd)) != hs->wdfnv) {
            fprintf(stderr, "[holar-assist] %s: project/r2_holar_lc.h is out of date (run tools/r2_holar_lc.py) - not patched\n", hs->name);
            return buf;
        }
        if (!holar_bytecode(p, n, hs->code, hs->coden, out)) { fprintf(stderr, "[holar-assist] %s: unexpected bytecode layout - not patched\n", hs->name); return buf; }
    } else if (!holar_text(p, n, hs->wd, out)) { fprintf(stderr, "[holar-assist] %s: main loop end not found - not patched\n", hs->name); return buf; }
    const uint32_t k = (uint32_t)std::min<size_t>(n, out.size());
    memcpy(vm_base + b, out.data(), k);
    put_size(se, k);
    if (out.size() > k) {
        std::lock_guard<std::mutex> lk(s_mu);
        s_streams.push_back(HolarStream{ z, b, n, std::move(out), k });
    }
    fprintf(stderr, "[holar-assist] %s: elevator watchdog added (R2_HOLAR_ASSIST=0 turns it off)\n", hs->name);
    return buf;
}

/* screen_out/prt lines from the watchdog: true = handled */
extern "C" int r2x_holar_prt(const char* msg)
{
    static const char tag[] = "holar-assist: ";
    if (strncmp(msg, tag, sizeof tag - 1) != 0) return 0;
    fprintf(stderr, "[holar-assist] %s\n", msg + sizeof tag - 1);
    return 1;
}