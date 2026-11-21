// Resistance 2 PC port: AVM1 (ActionScript 2) bytecode patcher for the "PC settings" pages of the
// audio/video options screen (scaleform/avoptions.swf in packed/game/global_cached.psarc) and their
// "PC SETTINGS" entry in the main menu's Options list (scaleform/mainmenu.swf).
//
// The screen (root timeline functions Init / Init_optionsMenu / keyPressInit, class
// _global.OptionsMenu) shows nine fixed rows [label, widget, value] and sends every change to the
// game as an FSCommand (SetEffectsVolume ... SetHelpTicker). The patch adds PC pages that reuse the
// same list, widgets and keys:
//
//   root:  pcPage = 0 (0 the AV list, 1..n the PC pages, n + 1 the category list), pcCat, the
//          category tables (pcCatOf / pcCatI per page, pcCatFirst / pcCatN / pcCatName per category);
//          PcRows() (the rows of the current page, generated from the native table), PcGo(np, q),
//          PcFlip(d, q), PcBack(), PcReset(), PcTitle(), PcHint(), PcOpen()
//   keyPressInit: PcOpen() -- FSCommand "PcStart": opened from the main menu's entry -> the category
//          list (pcSep: Back there leaves the screen); else the AV list, with "<L1> / <R1>: PC
//          SETTINGS" as its subtitle when there is no entry (pcEntry)
//   onKeyDown (keyPressInit): PcTitle() (the key names follow the device in use: FSCommand
//          "PcHint"); on the category list Enter / Right / R1 open the selected category's first
//          page (Left does nothing); Back -> PcBack() (a page -> the category list with that category
//          selected, the list -> the AV list unless pcSep); R1 / L1 -> PcFlip(+1 / -1) (pages wrap
//          1..n; not on the AV page when pcEntry); the game's Back -> PcReset() first (the AV values
//          are back in place before the screen closes)
//   PcGo: on entering the PC pages keeps the AV values (pcAV), gives optionsArray_Values a new
//          array, FSCommand "PcGet" <page> (native fills it), then Init_optionsMenu() rebuilds the list
//   OptionsMenu.Create: on a PC page the rows come from _root.PcRows(); a DynamicScroll widget takes
//          its start index from optionsArray_Values[row] (the game hard-codes row 5, its only one)
//   OptionsMenu.UpdateOptions: on a PC page sends FSCommand "PcSet" "<page>:<row>:<index>" instead
//          (nothing on the category list)
//
// mainmenu.swf: Options = [Controls, Audio/Visual, PC SETTINGS, PSP, Credits]; the new entry sends
// SetScreen "PcSettings" (r2_menurows.cpp opens AVOptions on the PC pages) and Localize() returns
// its label as it is.
//
// pausemenu.swf (in-game pause, campaign / co-op): Init adds [PC SETTINGS, pcsettings, <height>]
// after OPTIONS in pauseArray and makes the panel a row taller (PcEnd() first: the PC pages off).
// Enter on it sets pcWant and opens the game's own options panel (OpenMenu "options": the tab
// reads PC SETTINGS); Init_optionsMenu then shows the category list (pcSep) instead of the AV rows.
// The same tables, PcRows / PcGo / PcFlip / PcBack and OptionsMenu patches as avoptions.swf (the
// list is mainMenu.optionsList); PcTitle() names the page in the tab ("CONTROLS 1/2"); Back on a
// page -> the category list, on the list PcEnd() and the game's own Back closes the panel.
//
// All other bytes of the movies are kept. Branch and block offsets are recomputed after the
// insertions (decode -> action list with index targets -> encode).
#include "r2_swfpatch.h"
#include "miniz.h"
#include <cstring>
#include <cmath>
#include <initializer_list>
#include <functional>
#include <utility>

namespace r2swf {

namespace {

enum : uint8_t {
    OP_NOT = 0x12, OP_POP = 0x17, OP_GETVAR = 0x1C, OP_SETVAR = 0x1D, OP_DEFLOCAL = 0x3C, OP_CALLFUNC = 0x3D,
    OP_RETURN = 0x3E, OP_MODULO = 0x3F, OP_NEWOBJ = 0x40, OP_INITARRAY = 0x42, OP_TYPEOF = 0x44, OP_ADD2 = 0x47,
    OP_LESS2 = 0x48, OP_EQUALS2 = 0x49, OP_INCREMENT = 0x50, OP_GETMEMBER = 0x4E, OP_SETMEMBER = 0x4F, OP_CALLMETHOD = 0x52, OP_STRICTEQ = 0x66,
    OP_GREATER = 0x67, OP_GETURL = 0x83, OP_WAITFRAME = 0x8A, OP_WAITFRAME2 = 0x8D, OP_DEFFUNC2 = 0x8E,
    OP_TRY = 0x8F, OP_WITH = 0x94, OP_PUSH = 0x96, OP_JUMP = 0x99, OP_GETURL2 = 0x9A, OP_DEFFUNC = 0x9B,
    OP_IF = 0x9D,
};

void fail(std::string* why, const std::string& s) { if (why && why->empty()) *why = s; }
bool is_branch(uint8_t op) { return op == OP_IF || op == OP_JUMP; }
bool is_block(uint8_t op) { return op == OP_DEFFUNC || op == OP_DEFFUNC2 || op == OP_WITH; }
uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
void wr16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void put16(std::vector<uint8_t>& o, uint32_t v) { o.push_back((uint8_t)v); o.push_back((uint8_t)(v >> 8)); }
void put32(std::vector<uint8_t>& o, uint32_t v) { for (int i = 0; i < 4; i++) o.push_back((uint8_t)(v >> (8 * i))); }
void put_str(std::vector<uint8_t>& o, const std::string& s) { o.insert(o.end(), s.begin(), s.end()); o.push_back(0); }

/* ---- push values ---------------------------------------------------------------------------- */
struct V {
    char k = 'U';        // S string, N number, R register, U undefined, B boolean, L null, C constant
    std::string s;
    double n = 0;
    int r = 0;
};
V S(const std::string& s) { V v; v.k = 'S'; v.s = s; return v; }
/* the code's ConstantPool: Push values that refer to it (types 8 / 9) are matched as the strings */
std::vector<std::string> g_pool;
void load_pool(const std::vector<Act>& acts)
{
    g_pool.clear();
    for (const Act& a : acts) {
        if (a.op != 0x88 || a.data.size() < 2) continue;
        const size_t cnt = rd16(a.data.data());
        size_t j = 2;
        for (size_t k = 0; k < cnt && j < a.data.size(); k++) {
            size_t e = j; while (e < a.data.size() && a.data[e]) e++;
            g_pool.emplace_back((const char*)&a.data[j], e - j);
            j = e + 1;
        }
        return;
    }
}
V N(double n) { V v; v.k = 'N'; v.n = n; return v; }
V R(int r) { V v; v.k = 'R'; v.r = r; return v; }
V B(bool b) { V v; v.k = 'B'; v.n = b ? 1 : 0; return v; }
V U() { return V(); }

bool parse_push(const Act& a, std::vector<V>& out)
{
    out.clear();
    if (a.op != OP_PUSH) return false;
    const std::vector<uint8_t>& d = a.data;
    size_t j = 0;
    while (j < d.size()) {
        const uint8_t t = d[j++];
        V v;
        switch (t) {
        case 0: { size_t e = j; while (e < d.size() && d[e]) e++; if (e >= d.size()) return false;
                  v.k = 'S'; v.s.assign((const char*)&d[j], e - j); j = e + 1; break; }
        case 1: { if (j + 4 > d.size()) return false; uint32_t u = rd32(&d[j]); float f; memcpy(&f, &u, 4);
                  v.k = 'N'; v.n = f; j += 4; break; }
        case 2: v.k = 'L'; break;
        case 3: v.k = 'U'; break;
        case 4: if (j + 1 > d.size()) return false; v.k = 'R'; v.r = d[j++]; break;
        case 5: if (j + 1 > d.size()) return false; v.k = 'B'; v.n = d[j++]; break;
        case 6: { if (j + 8 > d.size()) return false;
                  uint64_t u = (uint64_t)rd32(&d[j]) << 32 | rd32(&d[j + 4]); double x; memcpy(&x, &u, 8);
                  v.k = 'N'; v.n = x; j += 8; break; }
        case 7: { if (j + 4 > d.size()) return false; v.k = 'N'; v.n = (double)(int32_t)rd32(&d[j]); j += 4; break; }
        case 8: if (j + 1 > d.size()) return false; v.k = 'C'; v.r = d[j++]; break;
        case 9: if (j + 2 > d.size()) return false; v.k = 'C'; v.r = rd16(&d[j]); j += 2; break;
        default: return false;
        }
        if (v.k == 'C' && v.r < (int)g_pool.size()) { v.k = 'S'; v.s = g_pool[(size_t)v.r]; }
        out.push_back(v);
    }
    return true;
}
void enc_value(std::vector<uint8_t>& o, const V& v)
{
    switch (v.k) {
    case 'S': o.push_back(0); put_str(o, v.s); break;
    case 'R': o.push_back(4); o.push_back((uint8_t)v.r); break;
    case 'B': o.push_back(5); o.push_back(v.n != 0); break;
    case 'L': o.push_back(2); break;
    case 'N':
        if (v.n == std::floor(v.n) && std::fabs(v.n) < 2147483647.0) { o.push_back(7); put32(o, (uint32_t)(int32_t)v.n); }
        else { o.push_back(6); uint64_t u; memcpy(&u, &v.n, 8); put32(o, (uint32_t)(u >> 32)); put32(o, (uint32_t)u); }
        break;
    default: o.push_back(3); break;
    }
}
// a Push whose values are exactly `want` (strings, numbers, registers)
bool push_is(const Act& a, std::initializer_list<V> want)
{
    std::vector<V> got;
    if (!parse_push(a, got) || got.size() != want.size()) return false;
    size_t i = 0;
    for (const V& w : want) {
        const V& g = got[i++];
        if (g.k != w.k) return false;
        if (w.k == 'S' && g.s != w.s) return false;
        if (w.k == 'N' && g.n != w.n) return false;
        if (w.k == 'R' && g.r != w.r) return false;
        if (w.k == 'B' && (g.n != 0) != (w.n != 0)) return false;
    }
    return true;
}
bool push_has(const Act& a, const std::string& s)
{
    std::vector<V> got;
    if (!parse_push(a, got)) return false;
    for (const V& g : got) if (g.k == 'S' && g.s == s) return true;
    return false;
}
std::string func_name(const Act& a)
{
    if ((a.op != OP_DEFFUNC && a.op != OP_DEFFUNC2) || a.data.empty()) return std::string();
    size_t e = 0;
    while (e < a.data.size() && a.data[e]) e++;
    return std::string((const char*)a.data.data(), e);
}

/* ---- assembler ------------------------------------------------------------------------------ */
struct Asm {
    std::vector<Act> a;
    std::vector<int> lab;
    std::vector<std::pair<size_t, int>> fix;
    int label() { lab.push_back(-1); return (int)lab.size() - 1; }
    void bind(int l) { lab[l] = (int)a.size(); }
    Asm& op(uint8_t o) { Act x; x.op = o; a.push_back(x); return *this; }
    Asm& push(std::initializer_list<V> vs)
    {
        Act x; x.op = OP_PUSH;
        for (const V& v : vs) enc_value(x.data, v);
        a.push_back(x);
        return *this;
    }
    Asm& br(uint8_t o, int l) { Act x; x.op = o; x.data = { 0, 0 }; a.push_back(x); fix.push_back({ a.size() - 1, l }); return *this; }
    Asm& geturl(const std::string& url, const std::string& target)
    {
        Act x; x.op = OP_GETURL; put_str(x.data, url); put_str(x.data, target); a.push_back(x);
        return *this;
    }
    Asm& geturl2() { Act x; x.op = OP_GETURL2; x.data = { 0 }; a.push_back(x); return *this; }
    // _root.<name>
    Asm& root(const std::string& name) { push({ S("_root") }).op(OP_GETVAR).push({ S(name) }).op(OP_GETMEMBER); return *this; }
    // _root.<name> = <the value the caller pushes in fn>
    template <class F> Asm& set_root(const std::string& name, F fn) { push({ S("_root") }).op(OP_GETVAR).push({ S(name) }); fn(); op(OP_SETMEMBER); return *this; }
    Asm& call0(const std::string& fn) { push({ N(0), S(fn) }).op(OP_CALLFUNC).op(OP_POP); return *this; }
    std::vector<Act> take()
    {
        for (auto& f : fix) a[f.first].target = lab[f.second];
        fix.clear();
        return std::move(a);
    }
    // function name(params) { body }  (DefineFunction, SWF 5 style: params are plain variables)
    Asm& func(const std::string& name, std::initializer_list<const char*> params, Asm& body)
    {
        std::vector<Act> b = body.take();
        Act x; x.op = OP_DEFFUNC;
        put_str(x.data, name);
        put16(x.data, (uint32_t)params.size());
        for (const char* p : params) put_str(x.data, p);
        put16(x.data, 0);
        const int base = (int)a.size() + 1;
        x.target = base + (int)b.size();
        a.push_back(x);
        for (Act& y : b) { if (y.target >= 0) y.target += base; a.push_back(std::move(y)); }
        return *this;
    }
};

/* ---- tags ----------------------------------------------------------------------------------- */
struct Tag { uint16_t code; size_t hdr, body, len; };
bool read_tags(const std::vector<uint8_t>& d, size_t pos, std::vector<Tag>& tags, std::string* why)
{
    while (pos + 2 <= d.size()) {
        Tag t;
        t.hdr = pos;
        const uint16_t h = rd16(&d[pos]); pos += 2;
        t.code = h >> 6;
        size_t len = h & 0x3F;
        if (len == 0x3F) {
            if (pos + 4 > d.size()) { fail(why, "truncated tag header"); return false; }
            len = rd32(&d[pos]); pos += 4;
        }
        if (pos + len > d.size()) { fail(why, "truncated tag"); return false; }
        t.body = pos; t.len = len;
        tags.push_back(t);
        pos += len;
        if (t.code == 0) return true;
    }
    fail(why, "no End tag");
    return false;
}
bool contains(const uint8_t* p, size_t n, const char* s)
{
    const size_t m = strlen(s) + 2;   // inline Push string: type 0, the text, its terminator
    std::string pat(1, '\0'); pat += s; pat += '\0';
    if (n < m) return false;
    for (size_t i = 0; i + m <= n; i++) if (!memcmp(p + i, pat.data(), m)) return true;
    return false;
}

/* ---- the generated code --------------------------------------------------------------------- */
const char* kHint = "L1 / R1: PC SETTINGS";   // the list's subtitle until PcTitle() has the device's keys
const char* kKeys = "L1 / R1";
const char* kHintSuffix = ": PC SETTINGS";
const char* kEntry = "PC SETTINGS";   // the main menu's Options entry
const char* kListHint = "CHOOSE A CATEGORY";

// the list Init_optionsMenu builds: _root.mainMenu.mc.optionsList (avoptions.swf), or
// _root.mainMenu.optionsList (pausemenu.swf, while g_pause is set)
bool g_pause = false;
void list_obj(Asm& f)
{
    f.root("mainMenu");
    if (!g_pause) f.push({ S("mc") }).op(OP_GETMEMBER);
    f.push({ S("optionsList") }).op(OP_GETMEMBER);
}
// _root.<arr>[_root.<idx> - 1]
void root_at(Asm& f, const char* arr, const char* idx) { f.root(arr).root(idx).push({ N(-1) }).op(OP_ADD2).op(OP_GETMEMBER); }
// _root.<name> = [v...]
void root_array(Asm& f, const char* name, const std::vector<double>& v)
{
    f.push({ S(name) });
    for (size_t i = v.size(); i-- > 0;) f.push({ N(v[i]) });
    f.push({ N((double)v.size()) }).op(OP_INITARRAY).op(OP_SETVAR);
}
void root_array(Asm& f, const char* name, const std::vector<std::string>& v)
{
    f.push({ S(name) });
    for (size_t i = v.size(); i-- > 0;) f.push({ S(v[i]) });
    f.push({ N((double)v.size()) }).op(OP_INITARRAY).op(OP_SETVAR);
}

// PcRows(): the rows of pcPage (1..n: the PC pages, n + 1: the category list)
void gen_pc_rows(Asm& f, const std::vector<std::vector<PcRow>>& pages)
{
    for (size_t p = 0; p < pages.size(); p++) {
        const int next = f.label();
        f.root("pcPage").push({ N((double)(p + 1)) }).op(OP_EQUALS2).op(OP_NOT).br(OP_IF, next);
        const std::vector<PcRow>& rows = pages[p];
        for (size_t r = rows.size(); r-- > 0;) {
            const PcRow& row = rows[r];
            for (size_t c = row.choices.size(); c-- > 0;) f.push({ S(row.choices[c]) });
            f.push({ N((double)row.choices.size()) }).op(OP_INITARRAY);
            f.push({ S("optionScroll"), S(row.label + " :") }).push({ N(3) }).op(OP_INITARRAY);
        }
        f.push({ N((double)rows.size()) }).op(OP_INITARRAY).op(OP_RETURN);
        f.bind(next);
    }
    f.push({ N(0) }).op(OP_INITARRAY).op(OP_RETURN);
}
// PcTitle(): the screen title and the subtitle (page keys for the device in use) for pcPage
//   AV list:   the game's title; "<L1> / <R1>: PC SETTINGS" (nothing when the main menu has the entry)
//   list:      "OPTIONS : PC SETTINGS :"  "CHOOSE A CATEGORY"
//   a page:    "OPTIONS : <CATEGORY> :"   ["PAGE i/n   "]"<L1> / <R1>: PAGES   *: AT NEXT START"
void gen_pc_title(Asm& f, size_t npages)
{
    const double list = (double)(npages + 1);
    const int go = f.label(), pc = f.label(), set = f.label(), none = f.label(), page = f.label(), nopg = f.label();
    const int pctitle = f.label(), titled = f.label(), named = f.label(), kept = f.label(), end = f.label();
    f.push({ S("t") }).root("mainMenu").push({ S("titleBox") }).op(OP_GETMEMBER).push({ S("subtitle_txt") }).op(OP_GETMEMBER).op(OP_DEFLOCAL);
    f.push({ S("t") }).op(OP_GETVAR).br(OP_IF, go);
    f.push({ U() }).op(OP_RETURN);
    f.bind(go);
    f.push({ S("tt") }).root("mainMenu").push({ S("titleBox") }).op(OP_GETMEMBER).push({ S("title_txt") }).op(OP_GETMEMBER).op(OP_DEFLOCAL);
    f.push({ S("k"), N(0), S("PcHint") }).op(OP_CALLFUNC).op(OP_DEFLOCAL);
    f.push({ S("t") }).op(OP_GETVAR).push({ S("autoSize"), S("left") }).op(OP_SETMEMBER);
    // the title
    f.push({ S("tt") }).op(OP_GETVAR).op(OP_NOT).br(OP_IF, titled);
    f.root("pcPage").push({ N(0) }).op(OP_EQUALS2).op(OP_NOT).br(OP_IF, pctitle);
    f.root("pcTitled").op(OP_NOT).br(OP_IF, titled);   // the AV list: the game's own title back
    f.push({ S("tt") }).op(OP_GETVAR).push({ S("text") }).root("pcT0").op(OP_SETMEMBER);
    f.root("goldTextFormat").push({ N(1), S("tt") }).op(OP_GETVAR).push({ S("setTextFormat") }).op(OP_CALLMETHOD).op(OP_POP);
    f.set_root("pcTitled", [&] { f.push({ B(false) }); });
    f.br(OP_JUMP, titled);
    f.bind(pctitle);
    f.root("pcTitled").br(OP_IF, kept);
    f.set_root("pcT0", [&] { f.push({ S("tt") }).op(OP_GETVAR).push({ S("text") }).op(OP_GETMEMBER); });
    f.bind(kept);
    f.push({ S("n"), S(kEntry) }).op(OP_DEFLOCAL);
    f.root("pcPage").push({ N(list) }).op(OP_EQUALS2).br(OP_IF, named);
    f.push({ S("n") });
    root_at(f, "pcCatName", "pcCat");
    f.op(OP_SETVAR);
    f.bind(named);
    f.push({ S("tt") }).op(OP_GETVAR).push({ S("text"), N(0), S("LOBBY_TITLE_OPTIONS"), N(1), S("Localize") }).op(OP_CALLFUNC)
        .push({ S("toUpperCase") }).op(OP_CALLMETHOD).push({ S(" : ") }).op(OP_ADD2).push({ S("n") }).op(OP_GETVAR).op(OP_ADD2)
        .push({ S(" :") }).op(OP_ADD2).op(OP_SETMEMBER);
    f.root("goldTextFormat").push({ N(1), S("tt") }).op(OP_GETVAR).push({ S("setTextFormat") }).op(OP_CALLMETHOD).op(OP_POP);
    f.set_root("pcTitled", [&] { f.push({ B(true) }); });
    f.bind(titled);
    // the subtitle
    f.push({ S("t") }).op(OP_GETVAR).push({ S("text") });
    f.root("pcPage").push({ N(0) }).op(OP_EQUALS2).op(OP_NOT).br(OP_IF, pc);
    // the AV list: the way to the pages, or nothing when they have their own entry in the main menu
    f.root("pcEntry").br(OP_IF, none);
    f.push({ S("k") }).op(OP_GETVAR).push({ S(kHintSuffix) }).op(OP_ADD2).br(OP_JUMP, set);
    f.bind(none);
    f.push({ S("") }).br(OP_JUMP, set);
    f.bind(pc);
    f.root("pcPage").push({ N(list) }).op(OP_EQUALS2).op(OP_NOT).br(OP_IF, page);
    f.push({ S(kListHint) }).br(OP_JUMP, set);
    f.bind(page);
    f.push({ S("") });
    root_at(f, "pcCatN", "pcCat");
    f.push({ N(1) }).op(OP_GREATER).op(OP_NOT).br(OP_IF, nopg);
    f.push({ S("PAGE ") }).op(OP_ADD2);
    root_at(f, "pcCatI", "pcPage");
    f.op(OP_ADD2).push({ S("/") }).op(OP_ADD2);
    root_at(f, "pcCatN", "pcCat");
    f.op(OP_ADD2).push({ S("   ") }).op(OP_ADD2);
    f.bind(nopg);
    f.push({ S("k") }).op(OP_GETVAR).op(OP_ADD2).push({ S(": PAGES   *: AT NEXT START") }).op(OP_ADD2);
    f.bind(set);
    f.op(OP_SETMEMBER);
    // the subtitle follows the title
    f.push({ S("tt") }).op(OP_GETVAR).op(OP_NOT).br(OP_IF, end);
    f.push({ S("t") }).op(OP_GETVAR).push({ S("_x"), S("tt") }).op(OP_GETVAR).push({ S("_x") }).op(OP_GETMEMBER)
        .push({ S("tt") }).op(OP_GETVAR).push({ S("_width") }).op(OP_GETMEMBER).op(OP_ADD2).op(OP_SETMEMBER);
    f.bind(end);
}
// PcHint(): the page keys for the device in use ("RMB / LMB", "LB / RB", ...) from FSCommand
// "PcHint" (native fills _root.pcHintC with character codes and a 0); "L1 / R1" without an answer
void gen_pc_hint(Asm& f)
{
    const int loop = f.label(), done = f.label(), ok = f.label();
    f.set_root("pcHintC", [&] { f.push({ N(0), S("Array") }).op(OP_NEWOBJ); });
    f.geturl("FSCommand:PcHint", "");
    f.push({ S("k"), S("") }).op(OP_DEFLOCAL);
    f.push({ S("i"), N(0) }).op(OP_DEFLOCAL);
    f.bind(loop);
    f.push({ S("i") }).op(OP_GETVAR).push({ N(40) }).op(OP_LESS2).op(OP_NOT).br(OP_IF, done);
    f.push({ S("c") }).root("pcHintC").push({ S("i") }).op(OP_GETVAR).op(OP_GETMEMBER).op(OP_DEFLOCAL);
    f.push({ S("c") }).op(OP_GETVAR).op(OP_TYPEOF).push({ S("number") }).op(OP_STRICTEQ).op(OP_NOT).br(OP_IF, done);
    f.push({ S("c") }).op(OP_GETVAR).push({ N(0) }).op(OP_GREATER).op(OP_NOT).br(OP_IF, done);
    f.push({ S("k"), S("k") }).op(OP_GETVAR).push({ S("c") }).op(OP_GETVAR).push({ N(1), S("String") }).op(OP_GETVAR)
        .push({ S("fromCharCode") }).op(OP_CALLMETHOD).op(OP_ADD2).op(OP_SETVAR);
    f.push({ S("i"), S("i") }).op(OP_GETVAR).op(OP_INCREMENT).op(OP_SETVAR);
    f.br(OP_JUMP, loop);
    f.bind(done);
    f.push({ S("k") }).op(OP_GETVAR).push({ S("") }).op(OP_EQUALS2).op(OP_NOT).br(OP_IF, ok);
    f.push({ S("k"), S(kKeys) }).op(OP_SETVAR);
    f.bind(ok);
    f.push({ S("k") }).op(OP_GETVAR).op(OP_RETURN);
}
// PcOpen() (after keyPressInit): FSCommand "PcStart" -> _root.pcStart = [opened from the main menu's
// "PC SETTINGS" entry, that entry exists]. Opened from the entry: straight to the category list
// (pcSep: Back there leaves the screen). Else the AV list, with L1 / R1 to the category list only
// when there is no entry (no answer = no entry: the pages stay reachable).
void gen_pc_open(Asm& f)
{
    const int legacy = f.label();
    f.set_root("pcStart", [&] { f.push({ N(0), S("Array") }).op(OP_NEWOBJ); });
    f.geturl("FSCommand:PcStart", "");
    f.set_root("pcEntry", [&] { f.root("pcStart").push({ N(1) }).op(OP_GETMEMBER).push({ N(1) }).op(OP_EQUALS2); });
    f.root("pcStart").push({ N(0) }).op(OP_GETMEMBER).push({ N(1) }).op(OP_EQUALS2).op(OP_NOT).br(OP_IF, legacy);
    f.set_root("pcSep", [&] { f.push({ N(1) }); });
    f.push({ N(1), N(1), N(2), S("PcFlip") }).op(OP_CALLFUNC).op(OP_POP);
    f.push({ U() }).op(OP_RETURN);
    f.bind(legacy);
    f.call0("PcTitle");
}
// PcGo(np, q): shows page np (0 the AV list, 1..n a PC page, n + 1 the category list). Leaving the AV
// list keeps its values (pcAV); a PC page gets a new optionsArray_Values, FSCommand "PcGet" <np>
// (native fills it) and pcCat = its category; then Init_optionsMenu() rebuilds the list. On the
// category list the category last shown stays selected. q: no page-turn sound.
void gen_pc_go(Asm& f, size_t npages)
{
    const double list = (double)(npages + 1);
    const int keep = f.label(), back = f.label(), end = f.label(), quiet = f.label(), noget = f.label(), nosel = f.label();
    // leaving the AV page: keep its values
    f.root("pcPage").push({ N(0) }).op(OP_EQUALS2).op(OP_NOT).br(OP_IF, keep);
    f.push({ S("np") }).op(OP_GETVAR).push({ N(0) }).op(OP_EQUALS2).br(OP_IF, keep);
    f.set_root("pcAV", [&] { f.root("optionsArray_Values"); });
    f.bind(keep);
    f.push({ S("np") }).op(OP_GETVAR).push({ N(0) }).op(OP_EQUALS2).br(OP_IF, back);
    f.set_root("optionsArray_Values", [&] { f.push({ N(0), S("Array") }).op(OP_NEWOBJ); });
    f.set_root("pcPage", [&] { f.push({ S("np") }).op(OP_GETVAR); });
    f.push({ S("np") }).op(OP_GETVAR).push({ N((double)npages) }).op(OP_GREATER).br(OP_IF, noget);
    f.set_root("pcCat", [&] { root_at(f, "pcCatOf", "pcPage"); });
    f.push({ S("FSCommand:PcGet"), S("np") }).op(OP_GETVAR).push({ S("") }).op(OP_ADD2).geturl2();
    f.bind(noget);
    for (int i = 0; i < 9; i++) {   // anything the game did not fill shows the first choice
        const int ok = f.label();
        f.root("optionsArray_Values").push({ N(i) }).op(OP_GETMEMBER).op(OP_TYPEOF).push({ S("number") }).op(OP_STRICTEQ).br(OP_IF, ok);
        f.root("optionsArray_Values").push({ N(i), N(0) }).op(OP_SETMEMBER);
        f.bind(ok);
    }
    f.br(OP_JUMP, end);
    f.bind(back);
    f.set_root("optionsArray_Values", [&] { f.root("pcAV"); });
    f.set_root("pcPage", [&] { f.push({ N(0) }); });
    f.bind(end);
    f.push({ S("q") }).op(OP_GETVAR).br(OP_IF, quiet);
    f.geturl("FSCommand:PlaySound", "ChangeOption");
    f.bind(quiet);
    f.call0("PcTitle").call0("Init_optionsMenu");
    f.root("pcPage").push({ N(list) }).op(OP_EQUALS2).op(OP_NOT).br(OP_IF, nosel);
    f.root("pcCat").push({ N(1) }).op(OP_LESS2).br(OP_IF, nosel);
    f.root("pcCat").push({ N(-1) }).op(OP_ADD2).push({ N(1) });
    list_obj(f);
    f.push({ S("ForceSelection") }).op(OP_CALLMETHOD).op(OP_POP);
    f.push({ N(0) });
    list_obj(f);
    f.push({ S("UpdateList") }).op(OP_CALLMETHOD).op(OP_POP);
    f.push({ N(0) });
    list_obj(f);
    f.push({ S("UpdateSelectionBox") }).op(OP_CALLMETHOD).op(OP_POP);
    f.bind(nosel);
}
// PcFlip(d, q): L1 / R1 (d -1 / +1). The category list: forward opens the selected category's first
// page, back returns to the AV list (not when opened from the entry). The AV list: the category
// list. A PC page: the previous / next page (1..n, wrapping).
void gen_pc_flip(Asm& f, size_t npages, size_t ncats)
{
    const double list = (double)(npages + 1);
    const int notlist = f.label(), toav = f.label(), done = f.label(), notav = f.label();
    const int s1 = f.label(), s2 = f.label(), s3 = f.label(), lo = f.label(), hi = f.label();
    f.root("pcPage").push({ N(list) }).op(OP_EQUALS2).op(OP_NOT).br(OP_IF, notlist);
    f.push({ S("d") }).op(OP_GETVAR).push({ N(0) }).op(OP_GREATER).op(OP_NOT).br(OP_IF, toav);
    f.push({ S("s") });
    list_obj(f);
    f.push({ S("currentSelection") }).op(OP_GETMEMBER).op(OP_DEFLOCAL);
    f.push({ S("s") }).op(OP_GETVAR).op(OP_TYPEOF).push({ S("number") }).op(OP_STRICTEQ).br(OP_IF, s1);
    f.push({ S("s"), N(0) }).op(OP_SETVAR);
    f.bind(s1);
    f.push({ S("s") }).op(OP_GETVAR).push({ N(0) }).op(OP_LESS2).op(OP_NOT).br(OP_IF, s2);
    f.push({ S("s"), N(0) }).op(OP_SETVAR);
    f.bind(s2);
    f.push({ S("s") }).op(OP_GETVAR).push({ N((double)ncats) }).op(OP_LESS2).br(OP_IF, s3);
    f.push({ S("s"), N(0) }).op(OP_SETVAR);
    f.bind(s3);
    f.push({ S("q") }).op(OP_GETVAR).root("pcCatFirst").push({ S("s") }).op(OP_GETVAR).op(OP_GETMEMBER)
        .push({ N(2), S("PcGo") }).op(OP_CALLFUNC).op(OP_POP);
    f.push({ U() }).op(OP_RETURN);
    f.bind(toav);
    f.root("pcSep").br(OP_IF, done);
    f.push({ S("q") }).op(OP_GETVAR).push({ N(0), N(2), S("PcGo") }).op(OP_CALLFUNC).op(OP_POP);
    f.bind(done);
    f.push({ U() }).op(OP_RETURN);
    f.bind(notlist);
    f.root("pcPage").push({ N(0) }).op(OP_EQUALS2).op(OP_NOT).br(OP_IF, notav);
    f.push({ S("q") }).op(OP_GETVAR).push({ N(list), N(2), S("PcGo") }).op(OP_CALLFUNC).op(OP_POP);
    f.push({ U() }).op(OP_RETURN);
    f.bind(notav);
    f.push({ S("np") }).root("pcPage").push({ S("d") }).op(OP_GETVAR).op(OP_ADD2).op(OP_DEFLOCAL);
    f.push({ S("np") }).op(OP_GETVAR).push({ N(1) }).op(OP_LESS2).op(OP_NOT).br(OP_IF, lo);
    f.push({ S("np"), N((double)npages) }).op(OP_SETVAR);
    f.bind(lo);
    f.push({ S("np") }).op(OP_GETVAR).push({ N((double)npages) }).op(OP_GREATER).op(OP_NOT).br(OP_IF, hi);
    f.push({ S("np"), N(1) }).op(OP_SETVAR);
    f.bind(hi);
    f.push({ S("q") }).op(OP_GETVAR).push({ S("np") }).op(OP_GETVAR).push({ N(2), S("PcGo") }).op(OP_CALLFUNC).op(OP_POP);
}
// PcBack(): Back on the options list. A PC page: the category list (true). The category list: the
// AV list when the screen was not opened from the entry (true). Else false: the game's Back.
void gen_pc_back(Asm& f, size_t npages)
{
    const double list = (double)(npages + 1);
    const int no = f.label(), page = f.label();
    f.root("pcPage").push({ N(0) }).op(OP_EQUALS2).br(OP_IF, no);
    f.root("pcPage").push({ N(list) }).op(OP_EQUALS2).op(OP_NOT).br(OP_IF, page);
    f.root("pcSep").br(OP_IF, no);
    f.geturl("FSCommand:PlaySound", "CursorBack");
    f.push({ N(1), N(0), N(2), S("PcGo") }).op(OP_CALLFUNC).op(OP_POP);
    f.push({ B(true) }).op(OP_RETURN);
    f.bind(page);
    f.geturl("FSCommand:PlaySound", "CursorBack");
    f.push({ N(1), N(list), N(2), S("PcGo") }).op(OP_CALLFUNC).op(OP_POP);
    f.push({ B(true) }).op(OP_RETURN);
    f.bind(no);
    f.push({ B(false) }).op(OP_RETURN);
}
void gen_pc_reset(Asm& f)
{
    const int end = f.label();
    f.root("pcPage").push({ N(0) }).op(OP_EQUALS2).br(OP_IF, end);
    f.set_root("optionsArray_Values", [&] { f.root("pcAV"); });
    f.set_root("pcPage", [&] { f.push({ N(0) }); });
    f.call0("PcTitle").call0("Init_optionsMenu");
    f.bind(end);
}
// pad / key test: PadValues.<fn>()
void pad_pressed(Asm& f, const char* fn) { f.push({ N(0), S("PadValues") }).op(OP_GETVAR).push({ S(fn) }).op(OP_CALLMETHOD); }

// The state, the page tables and the functions both movies share: pcCatOf[page - 1] = its category
// (1-based), pcCatI[page - 1] = its number in the category; pcCatFirst[cat - 1] = its first page,
// pcCatN = its page count, pcCatName / pcCatTab (the pause menu's tab: "DISPLAY", "CONTROLS", ...);
// PcRows, PcGo, PcFlip, PcBack.
void gen_tables(Asm& f, const std::vector<std::vector<PcRow>>& pages, const std::vector<PcCategory>& cats)
{
    const size_t np = pages.size();
    std::vector<double> cat_of(np), cat_i(np), first, count;
    std::vector<std::string> names, tabs;
    std::vector<std::vector<PcRow>> all = pages;
    std::vector<PcRow> list_rows;
    for (size_t c = 0; c < cats.size(); c++) {
        for (int p = 0; p < cats[c].count; p++) { cat_of[(size_t)(cats[c].first + p)] = (double)(c + 1); cat_i[(size_t)(cats[c].first + p)] = p + 1; }
        first.push_back(cats[c].first + 1);
        count.push_back(cats[c].count);
        names.push_back(cats[c].label);
        std::string tab = cats[c].label.compare(0, 3, "PC ") == 0 ? cats[c].label.substr(3) : cats[c].label;
        const size_t amp = tab.find(" &");
        if (amp != std::string::npos && amp > 0) tab.resize(amp);
        tabs.push_back(tab);
        PcRow r; r.label = cats[c].label; r.choices.push_back(kCategoryOpen);
        list_rows.push_back(r);
    }
    all.push_back(list_rows);   // page n + 1: the category list
    f.push({ S("pcPage"), N(0) }).op(OP_SETVAR);
    f.push({ S("pcSep"), N(0) }).op(OP_SETVAR);
    f.push({ S("pcCat"), N(0) }).op(OP_SETVAR);
    f.push({ S("pcEntry"), B(false) }).op(OP_SETVAR);
    f.push({ S("pcTitled"), B(false) }).op(OP_SETVAR);
    root_array(f, "pcCatOf", cat_of);
    root_array(f, "pcCatI", cat_i);
    root_array(f, "pcCatFirst", first);
    root_array(f, "pcCatN", count);
    root_array(f, "pcCatName", names);
    root_array(f, "pcCatTab", tabs);
    { Asm b; gen_pc_rows(b, all); f.func("PcRows", {}, b); }
    { Asm b; gen_pc_go(b, np); f.func("PcGo", { "np", "q" }, b); }
    { Asm b; gen_pc_flip(b, np, cats.size()); f.func("PcFlip", { "d", "q" }, b); }
    { Asm b; gen_pc_back(b, np); f.func("PcBack", {}, b); }
}

// The pause menu's PcTitle(): its tab (mainMenu.tab.tab_txt, "PC SETTINGS" from the side list's
// row) names the category on a page, with "i/n" when it has more than one page.
void gen_pause_title(Asm& f, size_t npages)
{
    const double list = (double)(npages + 1);
    const int go = f.label(), page = f.label(), one = f.label();
    f.push({ S("t") }).root("mainMenu").push({ S("tab") }).op(OP_GETMEMBER).push({ S("tab_txt") }).op(OP_GETMEMBER).op(OP_DEFLOCAL);
    f.push({ S("t") }).op(OP_GETVAR).br(OP_IF, go);
    f.push({ U() }).op(OP_RETURN);
    f.bind(go);
    f.root("pcPage").push({ N(0) }).op(OP_GREATER).br(OP_IF, page);
    f.push({ U() }).op(OP_RETURN);
    f.bind(page);
    f.push({ S("s") });
    root_at(f, "pcCatTab", "pcCat");
    f.op(OP_DEFLOCAL);
    f.root("pcPage").push({ N(list) }).op(OP_EQUALS2).op(OP_NOT).br(OP_IF, one);
    f.push({ S("s"), S(kEntry) }).op(OP_SETVAR);
    f.bind(one);
    {
        const int set = f.label();
        f.root("pcPage").push({ N(list) }).op(OP_EQUALS2).br(OP_IF, set);
        root_at(f, "pcCatN", "pcCat");
        f.push({ N(1) }).op(OP_GREATER).op(OP_NOT).br(OP_IF, set);
        f.push({ S("s"), S("s") }).op(OP_GETVAR).push({ S(" ") }).op(OP_ADD2);
        root_at(f, "pcCatI", "pcPage");
        f.op(OP_ADD2).push({ S("/") }).op(OP_ADD2);
        root_at(f, "pcCatN", "pcCat");
        f.op(OP_ADD2).op(OP_SETVAR);
        f.bind(set);
    }
    f.push({ S("t") }).op(OP_GETVAR).push({ S("text"), S("s") }).op(OP_GETVAR).op(OP_SETMEMBER);
}

// The pause menu's PcEnd(): leaving its options panel (Back on the category list) or a new Init:
// the game's own option values back, the PC pages off.
void gen_pause_end(Asm& f)
{
    const int keep = f.label();
    f.root("pcPage").push({ N(0) }).op(OP_GREATER).op(OP_NOT).br(OP_IF, keep);
    f.set_root("optionsArray_Values", [&] { f.root("pcAV"); });
    f.bind(keep);
    f.set_root("pcPage", [&] { f.push({ N(0) }); });
    f.set_root("pcSep", [&] { f.push({ N(0) }); });
    f.set_root("pcWant", [&] { f.push({ B(false) }); });
}

/* the patches of each piece of code */
bool patch_root(std::vector<Act>& acts, const std::vector<std::vector<PcRow>>& pages, const std::vector<PcCategory>& cats,
                std::string* why)
{
    const int n = (int)acts.size();
    const size_t np = pages.size();
    const double list = (double)(np + 1);
    load_pool(acts);
    int key_fn = -1, down = -1, back = -1, sub = -1;
    for (int i = 0; i < n; i++) {
        if (func_name(acts[i]) == "keyPressInit") key_fn = i;
        if (down < 0 && push_has(acts[i], "IsDownPressed")) down = i;
        if (acts[i].op == OP_GETURL && i + 1 < n && acts[i + 1].op == OP_GETURL) {
            static const char k1[] = "FSCommand:PlaySound\0CursorBack", k2[] = "FSCommand:SetScreen\0MainMenu";
            const auto& a = acts[i].data; const auto& b = acts[i + 1].data;
            if (a.size() >= sizeof k1 && !memcmp(a.data(), k1, sizeof k1) && b.size() >= sizeof k2 && !memcmp(b.data(), k2, sizeof k2))
                back = i;
        }
        if (i + 1 < n && push_is(acts[i], { S("subtitleText"), S("") }) && acts[i + 1].op == OP_DEFLOCAL) sub = i;
    }
    if (key_fn < 0 || down < 0 || back < 0 || sub < 0) {
        fail(why, std::string("root: not found:") + (key_fn < 0 ? " keyPressInit" : "") + (down < 0 ? " onKeyDown" : "") +
                  (back < 0 ? " Back" : "") + (sub < 0 ? " subtitleText" : ""));
        return false;
    }
    int on_key = -1;   // the function holding the key handler (innermost one around "IsDownPressed")
    for (int i = down - 1; i >= 0; i--)
        if ((acts[i].op == OP_DEFFUNC || acts[i].op == OP_DEFFUNC2) && acts[i].target > down) { on_key = i; break; }
    if (on_key < 0 || on_key <= key_fn || acts[key_fn].target < acts[on_key].target || back <= on_key || back >= acts[on_key].target) {
        fail(why, "root: unexpected key handler layout");
        return false;
    }
    { Asm f; f.push({ S("subtitleText"), S(kHint) }); acts[sub].data = f.a[0].data; }
    // from the highest index down, so the earlier positions stay valid
    { Asm f; f.call0("PcReset"); insert(acts, back, f.take()); }
    {
        Asm f;
        const int end = f.label(), l1 = f.label(), keys = f.label(), notlist = f.label(), open = f.label(), noleft = f.label();
        const int noback = f.label();
        f.call0("PcTitle");   // the hint follows the device in use
        f.push({ S("section") }).op(OP_GETVAR).push({ S("options") }).op(OP_EQUALS2).op(OP_NOT).br(OP_IF, end);
        // the category list: Enter / Right open the selected category, Left does nothing
        f.root("pcPage").push({ N(list) }).op(OP_EQUALS2).op(OP_NOT).br(OP_IF, notlist);
        pad_pressed(f, "IsEnterPressed");
        f.br(OP_IF, open);
        pad_pressed(f, "IsRightPressed");
        f.op(OP_NOT).br(OP_IF, noleft);
        f.bind(open);
        f.push({ N(0), N(1), N(2), S("PcFlip") }).op(OP_CALLFUNC).op(OP_POP).push({ U() }).op(OP_RETURN);
        f.bind(noleft);
        pad_pressed(f, "IsLeftPressed");
        f.op(OP_NOT).br(OP_IF, notlist);
        f.push({ U() }).op(OP_RETURN);
        f.bind(notlist);
        // Back: a PC page -> the category list, the list -> the AV list (unless opened from the entry)
        pad_pressed(f, "IsBackPressed");
        f.op(OP_NOT).br(OP_IF, noback);
        f.push({ N(0), S("PcBack") }).op(OP_CALLFUNC).op(OP_NOT).br(OP_IF, noback);
        f.push({ U() }).op(OP_RETURN);
        f.bind(noback);
        // the AV list has no way to the pages when the main menu has its own PC SETTINGS entry
        f.root("pcPage").push({ N(0) }).op(OP_EQUALS2).op(OP_NOT).br(OP_IF, keys);
        f.root("pcEntry").br(OP_IF, end);
        f.bind(keys);
        pad_pressed(f, "IsR1Pressed");
        f.op(OP_NOT).br(OP_IF, l1);
        f.push({ N(0), N(1), N(2), S("PcFlip") }).op(OP_CALLFUNC).op(OP_POP).push({ U() }).op(OP_RETURN);
        f.bind(l1);
        pad_pressed(f, "IsL1Pressed");
        f.op(OP_NOT).br(OP_IF, end);
        f.push({ N(0), N(-1), N(2), S("PcFlip") }).op(OP_CALLFUNC).op(OP_POP).push({ U() }).op(OP_RETURN);
        f.bind(end);
        insert(acts, on_key + 1, f.take());
    }
    { Asm f; f.call0("PcOpen"); insert(acts, key_fn + 1, f.take()); }
    {
        Asm f;
        gen_tables(f, pages, cats);
        { Asm b; gen_pc_reset(b); f.func("PcReset", {}, b); }
        { Asm b; gen_pc_title(b, np); f.func("PcTitle", {}, b); }
        { Asm b; gen_pc_hint(b); f.func("PcHint", {}, b); }
        { Asm b; gen_pc_open(b); f.func("PcOpen", {}, b); }
        insert(acts, 0, f.take());
    }
    return true;
}

// the DefineFunction2 assigned by "Push r2, <name>" ... SetMember in a class definition
int method(const std::vector<Act>& acts, const char* name)
{
    for (int i = 0; i + 1 < (int)acts.size(); i++)
        if (push_is(acts[i], { R(2), S(name) }) && acts[i + 1].op == OP_DEFFUNC2) return i + 1;
    return -1;
}
bool patch_options_menu(std::vector<Act>& acts, size_t npages, std::string* why)
{
    load_pool(acts);
    const int create = method(acts, "Create"), update = method(acts, "UpdateOptions");
    if (create < 0 || update < 0 || update < create) { fail(why, "OptionsMenu: Create / UpdateOptions not found"); return false; }
    int list_max = -1, scroll = -1;
    for (int i = create + 1; i < acts[create].target; i++) {
        if (list_max < 0 && push_is(acts[i], { N(0), R(1), S("UpdateListMax") })) list_max = i;
        if (scroll < 0 && i + 2 < acts[create].target && push_is(acts[i], { S("index"), R(2), S("optionsArray_Values") })
            && acts[i + 1].op == OP_GETMEMBER && push_is(acts[i + 2], { N(5) }))
            scroll = i + 2;
    }
    if (list_max < 0 || scroll < 0 || scroll < list_max) { fail(why, "OptionsMenu.Create: unexpected code"); return false; }
    { Asm f; f.push({ R(3) }); acts[scroll].data = f.a[0].data; }   // the row being built (r3), not row 5
    {
        Asm f;
        const int end = f.label(), list = f.label();
        f.root("pcPage").push({ N(0) }).op(OP_GREATER).op(OP_NOT).br(OP_IF, end);
        // the category list (page n + 1) has nothing to set
        f.root("pcPage").push({ N((double)npages) }).op(OP_GREATER).br(OP_IF, list);
        f.push({ S("FSCommand:PcSet") }).root("pcPage").push({ S(":") }).op(OP_ADD2)
            .push({ R(1), S("currentSelection") }).op(OP_GETMEMBER).op(OP_ADD2).push({ S(":") }).op(OP_ADD2)
            .root("optionsArray_Values").push({ R(1), S("currentSelection") }).op(OP_GETMEMBER).op(OP_GETMEMBER).op(OP_ADD2)
            .geturl2();
        f.bind(list);
        f.push({ U() }).op(OP_RETURN);
        f.bind(end);
        insert(acts, update + 1, f.take());
    }
    {
        Asm f;
        const int end = f.label();
        f.root("pcPage").push({ N(0) }).op(OP_GREATER).op(OP_NOT).br(OP_IF, end);
        f.push({ R(1), S("optionsArray"), N(0), S("_root") }).op(OP_GETVAR).push({ S("PcRows") }).op(OP_CALLMETHOD).op(OP_SETMEMBER);
        f.bind(end);
        insert(acts, list_max, f.take());
    }
    return true;
}

/* mainmenu.swf, root DoAction: Localize(tag) and the lists (see patch_mainmenu) */
bool patch_main_root(std::vector<Act>& acts, std::string* why)
{
    load_pool(acts);
    const int n = (int)acts.size();
    int loc = -1, av = -1;
    for (int i = 0; i < n; i++) {
        if (loc < 0 && acts[i].op == OP_DEFFUNC2 && func_name(acts[i]) == "Localize") loc = i;
        if (i + 5 < n && push_is(acts[i], { B(true), S("AVOptions"), S("SetScreen"), S("LOBBY_OPTION_AUDIOVISUAL"), N(4) })
            && acts[i + 1].op == OP_INITARRAY
            && push_is(acts[i + 2], { B(true), S("Controls"), S("SetScreen"), S("LOBBY_OPTION_CONTROLS"), N(4) })
            && acts[i + 3].op == OP_INITARRAY && push_is(acts[i + 4], { N(4) }) && acts[i + 5].op == OP_INITARRAY) {
            if (av >= 0) { fail(why, "mainmenu: two Options lists"); return false; }
            av = i;
        }
    }
    if (loc < 0 || av < 0 || loc > av) {
        fail(why, std::string("mainmenu: not found:") + (loc < 0 ? " Localize" : "") + (av < 0 ? " the Options list (Controls, Audio/Visual, ...)" : ""));
        return false;
    }
    // Localize(r1:tag): one parameter, in register 1
    {
        const std::vector<uint8_t>& d = acts[loc].data;
        size_t j = 0;
        while (j < d.size() && d[j]) j++;
        j++;
        if (j + 5 + 2 > d.size() || rd16(&d[j]) != 1 || d[j + 5] != 1) { fail(why, "mainmenu: Localize is not Localize(r1)"); return false; }
    }
    // the group's entry count: Controls, Audio/Visual, PC SETTINGS, PSP, Credits (the list shows 5)
    { Asm f; f.push({ N(5) }); acts[av + 4].data = f.a[0].data; }
    {
        // pushed before Audio/Visual = after it in the list. The movie sends FSCommand SetScreen
        // "PcSettings"; the native side opens AVOptions on the PC pages (r2_menurows.cpp)
        Asm f;
        f.push({ B(true), S("PcSettings"), S("SetScreen"), S(kEntry), N(4) }).op(OP_INITARRAY);
        insert(acts, av, f.take());
    }
    {
        // the list's labels are tags for Localize(); ours is the text itself
        Asm f;
        const int end = f.label();
        f.push({ R(1), S(kEntry) }).op(OP_EQUALS2).op(OP_NOT).br(OP_IF, end);
        f.push({ R(1) }).op(OP_RETURN);
        f.bind(end);
        insert(acts, loc + 1, f.take());
    }
    return true;
}

const char* kPauseSection = "pcsettings";   // the new pauseArray entry's section (opens "options" on the PC pages)

/* pausemenu.swf, root DoAction: Init builds pauseArray ([label, section, panel height] per game
 * mode), then FrameUpdate(r1[gameMode + "Scale"]) sizes the panel, Parse(optionsArray, ...); the
 * Enter key on the side list (section "pause") opens pauseArray[selection][1] (OpenMenu: the panel
 * grows to the row's height, the tab shows its label, then Init_<section>Menu()). The game's own
 * options panel (Init_optionsMenu: the OptionsMenu list mainMenu.optionsList, the same class as
 * avoptions.swf's) hosts the PC pages: PC SETTINGS opens it with _root.pcWant set, and
 * Init_optionsMenu then shows the category list (pcSep: Back there closes the panel). */
bool patch_pause_root(std::vector<Act>& acts, const std::vector<std::vector<PcRow>>& pages, const std::vector<PcCategory>& cats,
                      std::string* why)
{
    load_pool(acts);
    const int n = (int)acts.size();
    const size_t np = pages.size();
    const double list = (double)(np + 1);
    int frame = -1, enter = -1, key_fn = -1, down = -1, init_opt = -1;
    for (int i = 0; i < n; i++) {
        if (push_has(acts[i], kPauseSection)) { fail(why, "pausemenu: already has the PC SETTINGS entry"); return false; }
        if (func_name(acts[i]) == "keyPressInit") key_fn = i;
        if (func_name(acts[i]) == "Init_optionsMenu") init_opt = i;
        if (down < 0 && key_fn >= 0 && push_has(acts[i], "IsDownPressed")) down = i;
        if (i + 8 < n && push_is(acts[i], { R(1), S("gameMode") }) && acts[i + 1].op == OP_GETVAR
            && push_is(acts[i + 2], { S("Scale") }) && acts[i + 3].op == OP_ADD2 && acts[i + 4].op == OP_GETMEMBER
            && push_is(acts[i + 5], { N(1), S("FrameUpdate") }) && acts[i + 6].op == OP_CALLFUNC && acts[i + 7].op == OP_POP
            && push_is(acts[i + 8], { N(2), S("optionsArray"), S("optionsArray_Values"), N(3), S("Parse") })) {
            if (frame >= 0) { fail(why, "pausemenu: two Init panels"); return false; }
            frame = i;
        }
        if (i + 11 < n && push_is(acts[i], { N(0), S("PadValues") }) && acts[i + 1].op == OP_GETVAR
            && push_is(acts[i + 2], { S("IsEnterPressed") }) && acts[i + 3].op == OP_CALLMETHOD
            && acts[i + 4].op == OP_NOT && acts[i + 5].op == OP_IF
            && push_is(acts[i + 6], { S("section") }) && acts[i + 7].op == OP_GETVAR
            && push_is(acts[i + 8], { S("pause") }) && acts[i + 9].op == OP_EQUALS2
            && acts[i + 10].op == OP_NOT && acts[i + 11].op == OP_IF) {
            if (enter >= 0) { fail(why, "pausemenu: two Enter handlers"); return false; }
            enter = i + 12;
        }
    }
    if (frame < 0 || enter < 0 || key_fn < 0 || down < 0 || init_opt < 0) {
        fail(why, std::string("pausemenu: not found:") + (frame < 0 ? " Init's panel / pauseArray" : "") + (enter < 0 ? " the side list's Enter key" : "")
                  + (key_fn < 0 ? " keyPressInit" : "") + (down < 0 ? " onKeyDown" : "") + (init_opt < 0 ? " Init_optionsMenu" : ""));
        return false;
    }
    int on_key = -1;   // the function holding the key handler (innermost one around "IsDownPressed")
    for (int i = down - 1; i >= 0; i--)
        if ((acts[i].op == OP_DEFFUNC || acts[i].op == OP_DEFFUNC2) && acts[i].target > down) { on_key = i; break; }
    if (on_key < 0 || on_key <= key_fn || acts[key_fn].target < acts[on_key].target || enter <= on_key || enter >= acts[on_key].target
        || enter < frame || init_opt < frame || init_opt > on_key) {
        fail(why, "pausemenu: unexpected code layout");
        return false;
    }
    // from the highest index down, so the earlier positions stay valid
    {
        // Enter on the side list: PC SETTINGS opens the game's options panel (row label in the tab,
        // row height) and Init_optionsMenu shows the PC category list
        Asm f;
        const int end = f.label();
        f.push({ S("pauseArray") }).op(OP_GETVAR).push({ S("mainMenu") }).op(OP_GETVAR).push({ S("pauseList") }).op(OP_GETMEMBER)
            .push({ S("currentSelection") }).op(OP_GETMEMBER).op(OP_GETMEMBER).push({ N(1) }).op(OP_GETMEMBER)
            .push({ S(kPauseSection) }).op(OP_EQUALS2).op(OP_NOT).br(OP_IF, end);
        f.set_root("pcWant", [&] { f.push({ B(true) }); });
        f.push({ S("options"), N(1), S("OpenMenu") }).op(OP_CALLFUNC).op(OP_POP);
        f.geturl("FSCommand:PlaySound", "CursorSelect");
        f.push({ U() }).op(OP_RETURN);
        f.bind(end);
        insert(acts, enter, f.take());
    }
    {
        // the options panel on a PC page / the category list (section "options")
        Asm f;
        const int end = f.label(), notlist = f.label(), open = f.label(), noleft = f.label(), noback = f.label(), l1 = f.label();
        f.push({ S("section") }).op(OP_GETVAR).push({ S("options") }).op(OP_EQUALS2).op(OP_NOT).br(OP_IF, end);
        f.root("pcPage").push({ N(0) }).op(OP_GREATER).op(OP_NOT).br(OP_IF, end);
        f.root("pcPage").push({ N(list) }).op(OP_EQUALS2).op(OP_NOT).br(OP_IF, notlist);
        pad_pressed(f, "IsEnterPressed");
        f.br(OP_IF, open);
        pad_pressed(f, "IsRightPressed");
        f.op(OP_NOT).br(OP_IF, noleft);
        f.bind(open);
        f.push({ N(0), N(1), N(2), S("PcFlip") }).op(OP_CALLFUNC).op(OP_POP).push({ U() }).op(OP_RETURN);
        f.bind(noleft);
        pad_pressed(f, "IsLeftPressed");
        f.op(OP_NOT).br(OP_IF, notlist);
        f.push({ U() }).op(OP_RETURN);
        f.bind(notlist);
        // Back: a page -> the category list; the list -> the game's own Back (closes the panel)
        pad_pressed(f, "IsBackPressed");
        f.op(OP_NOT).br(OP_IF, noback);
        {
            const int closing = f.label();
            f.push({ N(0), S("PcBack") }).op(OP_CALLFUNC).op(OP_NOT).br(OP_IF, closing);
            f.push({ U() }).op(OP_RETURN);
            f.bind(closing);
            f.call0("PcEnd");
            f.br(OP_JUMP, end);
        }
        f.bind(noback);
        pad_pressed(f, "IsR1Pressed");
        f.op(OP_NOT).br(OP_IF, l1);
        f.push({ N(0), N(1), N(2), S("PcFlip") }).op(OP_CALLFUNC).op(OP_POP).push({ U() }).op(OP_RETURN);
        f.bind(l1);
        pad_pressed(f, "IsL1Pressed");
        f.op(OP_NOT).br(OP_IF, end);
        f.push({ N(0), N(-1), N(2), S("PcFlip") }).op(OP_CALLFUNC).op(OP_POP).push({ U() }).op(OP_RETURN);
        f.bind(end);
        insert(acts, on_key + 1, f.take());
    }
    {
        // Init_optionsMenu: asked for by PC SETTINGS -> the category list instead of the game's rows
        Asm f;
        const int end = f.label();
        f.root("pcWant").op(OP_NOT).br(OP_IF, end);
        f.set_root("pcWant", [&] { f.push({ B(false) }); });
        f.set_root("pcSep", [&] { f.push({ N(1) }); });
        f.push({ B(true), N(list), N(2), S("PcGo") }).op(OP_CALLFUNC).op(OP_POP);
        f.push({ U() }).op(OP_RETURN);
        f.bind(end);
        insert(acts, init_opt + 1, f.take());
    }
    {
        // Init, before the panel is sized: the PC pages off (PcEnd), then
        // [PC SETTINGS, pcsettings, <OPTIONS' height>] after OPTIONS
        // (if this mode's list has it), and the panel one row (31) taller. Idempotent if Init runs
        // again on the same movie: no second row when the one after OPTIONS is already ours, and the
        // height only grows when it is not the value we set last time (_root["pcScale" + gameMode]).
        Asm f;
        const int loop = f.label(), next = f.label(), scale = f.label(), end = f.label();
        const auto scale_key = [&] { f.push({ S("_root") }).op(OP_GETVAR).push({ S("pcScale"), S("gameMode") }).op(OP_GETVAR).op(OP_ADD2); };
        const auto scale_ref = [&] { f.push({ R(1), S("gameMode") }).op(OP_GETVAR).push({ S("Scale") }).op(OP_ADD2); };
        f.call0("PcEnd");
        f.set_root("pcI", [&] { f.push({ N(0) }); });
        f.bind(loop);
        f.root("pcI").push({ S("pauseArray") }).op(OP_GETVAR).push({ S("length") }).op(OP_GETMEMBER).op(OP_LESS2).op(OP_NOT).br(OP_IF, end);
        f.push({ S("pauseArray") }).op(OP_GETVAR).root("pcI").op(OP_GETMEMBER).push({ N(1) }).op(OP_GETMEMBER)
            .push({ S("options") }).op(OP_EQUALS2).op(OP_NOT).br(OP_IF, next);
        f.push({ S("pauseArray") }).op(OP_GETVAR).root("pcI").push({ N(1) }).op(OP_ADD2).op(OP_GETMEMBER).push({ N(1) }).op(OP_GETMEMBER)
            .push({ S(kPauseSection) }).op(OP_EQUALS2).br(OP_IF, scale);
        f.push({ S("pauseArray") }).op(OP_GETVAR).root("pcI").op(OP_GETMEMBER).push({ N(2) }).op(OP_GETMEMBER)
            .push({ S(kPauseSection), S(kEntry), N(3) }).op(OP_INITARRAY)
            .push({ N(0) }).root("pcI").push({ N(1) }).op(OP_ADD2)
            .push({ N(3), S("pauseArray") }).op(OP_GETVAR).push({ S("splice") }).op(OP_CALLMETHOD).op(OP_POP);
        f.bind(scale);
        scale_key(); f.op(OP_GETMEMBER);
        scale_ref(); f.op(OP_GETMEMBER);
        f.op(OP_EQUALS2).br(OP_IF, end);
        scale_key(); scale_ref(); f.op(OP_GETMEMBER).push({ N(31) }).op(OP_ADD2).op(OP_SETMEMBER);
        scale_ref(); scale_key(); f.op(OP_GETMEMBER).op(OP_SETMEMBER);
        f.br(OP_JUMP, end);
        f.bind(next);
        f.set_root("pcI", [&] { f.root("pcI").op(OP_INCREMENT); });
        f.br(OP_JUMP, loop);
        f.bind(end);
        insert(acts, frame, f.take());
    }
    {
        Asm f;
        gen_tables(f, pages, cats);
        f.push({ S("pcWant"), B(false) }).op(OP_SETVAR);
        { Asm b; gen_pause_title(b, np); f.func("PcTitle", {}, b); }
        { Asm b; gen_pause_end(b); f.func("PcEnd", {}, b); }
        insert(acts, 0, f.take());
    }
    return true;
}

/* the one DoAction (tag 12) holding the raw bytes `marker` (inline or ConstantPool string), patched by fn;
 * with cls, also the one DoInitAction (tag 59) holding the strings "OptionsMenu" and "UpdateOptions"
 * (the OptionsMenu class), patched by cls */
using ActsFn = std::function<bool(std::vector<Act>&, std::string*)>;
bool has_str(const uint8_t* b, size_t n, const char* s)
{
    const size_t m = strlen(s) + 1;
    for (size_t i = 1; i + m <= n; i++) if (!b[i - 1] && !memcmp(b + i, s, m)) return true;
    return false;
}
bool patch_root_doaction(const std::vector<uint8_t>& in, const char* marker, const char* what, const ActsFn& fn,
                         std::vector<uint8_t>& out, std::string* why, const ActsFn& cls = ActsFn())
{
    if (why) why->clear();
    // a zlib-compressed movie ("CWS", or Scaleform's "CFX") is patched as, and written back as,
    // the uncompressed one ("FWS" / "GFX"); "GFX" is read as "FWS"
    std::vector<uint8_t> unz;
    const bool gfx = in.size() >= 8 && (!memcmp(in.data(), "GFX", 3) || !memcmp(in.data(), "CFX", 3));
    if (in.size() >= 8 && (!memcmp(in.data(), "CWS", 3) || !memcmp(in.data(), "CFX", 3))) {
        const uint32_t total = rd32(&in[4]);
        if (total < 8 || total > (64u << 20)) { fail(why, "bad CWS length"); return false; }
        unz.assign(in.begin(), in.begin() + 8);
        unz[0] = gfx ? 'G' : 'F';
        unz.resize(total);
        mz_ulong n = total - 8;
        if (mz_uncompress(unz.data() + 8, &n, in.data() + 8, (mz_ulong)(in.size() - 8)) != MZ_OK || n != total - 8) {
            fail(why, "bad CWS data"); return false;
        }
    }
    const std::vector<uint8_t>& swf = unz.empty() ? in : unz;
    if (swf.size() < 21 || (memcmp(swf.data(), "FWS", 3) && memcmp(swf.data(), "GFX", 3)) || swf[3] < 6) {
        fail(why, "not an SWF / GFx 6+ movie (" + std::string((const char*)swf.data(), swf.size() < 3 ? swf.size() : 3) + ")");
        return false;
    }
    if (rd32(&swf[4]) != swf.size()) { fail(why, "SWF length mismatch"); return false; }
    const unsigned nbits = swf[8] >> 3;
    const size_t start = 8 + (5 + 4 * nbits + 7) / 8 + 4;
    std::vector<Tag> tags;
    if (start >= swf.size() || !read_tags(swf, start, tags, why)) { fail(why, "bad SWF"); return false; }
    const size_t mlen = strlen(marker) + 1;
    int root = -1;
    for (int t = 0; t < (int)tags.size(); t++) {
        if (tags[t].code != 12) continue;
        const uint8_t* b = swf.data() + tags[t].body;
        const size_t n = tags[t].len;
        bool hit = false;
        for (size_t i = 0; !hit && i + mlen <= n; i++) hit = !memcmp(b + i, marker, mlen);
        if (!hit) continue;
        if (root >= 0) { fail(why, std::string(what) + ": two root scripts"); return false; }
        root = t;
    }
    if (root < 0) { fail(why, std::string(what) + ": the script was not found (a different " + what + ".swf?)"); return false; }
    int ci = -1;
    if (cls) {
        for (int t = 0; t < (int)tags.size(); t++) {
            if (tags[t].code != 59 || tags[t].len < 2) continue;
            const uint8_t* b = swf.data() + tags[t].body;
            if (!has_str(b, tags[t].len, "OptionsMenu") || !has_str(b, tags[t].len, "UpdateOptions")) continue;
            if (ci >= 0) { fail(why, std::string(what) + ": two OptionsMenu classes"); return false; }
            ci = t;
        }
        if (ci < 0) { fail(why, std::string(what) + ": the OptionsMenu class was not found"); return false; }
    }
    std::vector<Act> ra, ca;
    std::vector<uint8_t> rcode, ccode;
    if (!decode(swf.data() + tags[root].body, tags[root].len, ra, why)) return false;
    if (!fn(ra, why) || !encode(ra, rcode, why)) return false;
    if (ci >= 0) {
        if (!decode(swf.data() + tags[ci].body + 2, tags[ci].len - 2, ca, why)) return false;
        if (!cls(ca, why) || !encode(ca, ccode, why)) return false;
    }
    out.assign(swf.begin(), swf.begin() + start);
    for (int t = 0; t < (int)tags.size(); t++) {
        const Tag& g = tags[t];
        if (t != root && t != ci) { out.insert(out.end(), swf.begin() + g.hdr, swf.begin() + g.body + g.len); continue; }
        std::vector<uint8_t> body;
        if (t == ci) body.assign(swf.begin() + g.body, swf.begin() + g.body + 2);   // sprite id
        const std::vector<uint8_t>& code = t == root ? rcode : ccode;
        body.insert(body.end(), code.begin(), code.end());
        put16(out, (uint32_t)g.code << 6 | 0x3F);
        put32(out, (uint32_t)body.size());
        out.insert(out.end(), body.begin(), body.end());
    }
    if (tags.back().hdr + 2 + tags.back().len < swf.size())
        out.insert(out.end(), swf.begin() + tags.back().body + tags.back().len, swf.end());
    const uint32_t len = (uint32_t)out.size();
    for (int i = 0; i < 4; i++) out[4 + i] = (uint8_t)(len >> (8 * i));
    return true;
}

bool check_pages(const std::vector<std::vector<PcRow>>& pages, const std::vector<PcCategory>& cats, std::string* why)
{
    if (pages.empty()) { fail(why, "no pages"); return false; }
    for (const auto& p : pages) {
        if (p.empty() || p.size() > 9) { fail(why, "a page needs 1..9 rows"); return false; }
        for (const auto& r : p) if (r.choices.empty()) { fail(why, "a row without choices"); return false; }
    }
    // the categories: the list's rows (at most 9), each a run of pages, in order, covering them all
    if (cats.empty() || cats.size() > 9) { fail(why, "1..9 categories needed"); return false; }
    int next = 0;
    for (const auto& c : cats) {
        if (c.label.empty() || c.first != next || c.count < 1) { fail(why, "the categories do not cover the pages in order"); return false; }
        next += c.count;
    }
    if (next != (int)pages.size()) { fail(why, "the categories do not cover the pages in order"); return false; }
    return true;
}

} // namespace

const char kCategoryOpen[] = "OPEN";

bool root_actions(const std::vector<uint8_t>& swf, const char* marker, std::vector<Act>& acts, std::string* why)
{
    std::vector<uint8_t> unused;
    return patch_root_doaction(swf, marker, "movie", [&](std::vector<Act>& ra, std::string*) { acts = ra; return true; }, unused, why);
}

bool decode(const uint8_t* code, size_t n, std::vector<Act>& out, std::string* why)
{
    out.clear();
    std::vector<size_t> offs;
    size_t i = 0;
    while (i < n) {
        Act a;
        offs.push_back(i);
        a.op = code[i++];
        if (a.op >= 0x80) {
            if (i + 2 > n) { fail(why, "truncated action"); return false; }
            const size_t len = rd16(code + i); i += 2;
            if (i + len > n) { fail(why, "truncated action data"); return false; }
            a.data.assign(code + i, code + i + len);
            i += len;
        }
        if (a.op == OP_TRY || a.op == OP_WAITFRAME || a.op == OP_WAITFRAME2) { fail(why, "unsupported action (Try / WaitForFrame)"); return false; }
        out.push_back(std::move(a));
    }
    offs.push_back(n);
    for (size_t k = 0; k < out.size(); k++) {
        Act& a = out[k];
        long long to;
        const size_t after = offs[k + 1];
        if (is_branch(a.op)) {
            if (a.data.size() != 2) { fail(why, "bad branch"); return false; }
            to = (long long)after + (int16_t)rd16(a.data.data());
        } else if (is_block(a.op)) {
            if (a.data.size() < 2) { fail(why, "bad block"); return false; }
            to = (long long)after + rd16(a.op == OP_WITH ? a.data.data() : a.data.data() + a.data.size() - 2);
        } else continue;
        size_t lo = 0, hi = offs.size();
        while (lo < hi) { const size_t m = (lo + hi) / 2; if ((long long)offs[m] < to) lo = m + 1; else hi = m; }
        if (lo >= offs.size() || (long long)offs[lo] != to) { fail(why, "branch into the middle of an action"); return false; }
        a.target = (int)lo;
    }
    return true;
}

bool encode(const std::vector<Act>& acts, std::vector<uint8_t>& out, std::string* why)
{
    std::vector<size_t> offs(acts.size() + 1);
    size_t o = 0;
    for (size_t k = 0; k < acts.size(); k++) {
        offs[k] = o;
        o += acts[k].op >= 0x80 ? 3 + acts[k].data.size() : 1;
        if (acts[k].op >= 0x80 && acts[k].data.size() > 0xFFFF) { fail(why, "action too long"); return false; }
    }
    offs[acts.size()] = o;
    out.clear();
    out.reserve(o);
    for (size_t k = 0; k < acts.size(); k++) {
        const Act& a = acts[k];
        out.push_back(a.op);
        if (a.op < 0x80) continue;
        std::vector<uint8_t> d = a.data;
        if (is_branch(a.op) || is_block(a.op)) {
            if (a.target < 0 || a.target > (int)acts.size()) { fail(why, "branch without a target"); return false; }
            const long long delta = (long long)offs[a.target] - (long long)offs[k + 1];
            if (is_branch(a.op)) {
                if (delta < -32768 || delta > 32767 || d.size() != 2) { fail(why, "branch out of range"); return false; }
                wr16(d.data(), (uint32_t)(uint16_t)(int16_t)delta);
            } else {
                if (delta < 0 || delta > 0xFFFF || d.size() < 2) { fail(why, "block too large"); return false; }
                wr16(a.op == OP_WITH ? d.data() : d.data() + d.size() - 2, (uint32_t)delta);
            }
        }
        put16(out, (uint32_t)d.size());
        out.insert(out.end(), d.begin(), d.end());
    }
    return true;
}

void insert(std::vector<Act>& acts, int pos, std::vector<Act> ins)
{
    const int k = (int)ins.size();
    for (Act& a : acts) if (a.target > pos) a.target += k;
    for (Act& a : ins) if (a.target >= 0) a.target += pos;
    acts.insert(acts.begin() + pos, std::make_move_iterator(ins.begin()), std::make_move_iterator(ins.end()));
}

bool patch_avoptions(const std::vector<uint8_t>& swf, const std::vector<std::vector<PcRow>>& pages,
                     const std::vector<PcCategory>& cats, std::vector<uint8_t>& out, std::string* why)
{
    if (why) why->clear();
    if (!check_pages(pages, cats, why)) return false;
    if (swf.size() < 21 || memcmp(swf.data(), "FWS", 3) || swf[3] < 6) { fail(why, "not an uncompressed SWF 6+ movie"); return false; }
    if (rd32(&swf[4]) != swf.size()) { fail(why, "SWF length mismatch"); return false; }
    const unsigned nbits = swf[8] >> 3;
    const size_t start = 8 + (5 + 4 * nbits + 7) / 8 + 4;
    std::vector<Tag> tags;
    if (start >= swf.size() || !read_tags(swf, start, tags, why)) { fail(why, "bad SWF"); return false; }

    int root = -1, cls = -1;
    for (int t = 0; t < (int)tags.size(); t++) {
        const uint8_t* b = swf.data() + tags[t].body;
        const size_t n = tags[t].len;
        if (tags[t].code == 12 && contains(b, n, "Init_optionsMenu") && contains(b, n, "keyPressInit")) {
            if (root >= 0) { fail(why, "two root scripts"); return false; }
            root = t;
        }
        if (tags[t].code == 59 && contains(b, n, "OptionsMenu") && contains(b, n, "UpdateOptions")) {
            if (cls >= 0) { fail(why, "two OptionsMenu classes"); return false; }
            cls = t;
        }
    }
    if (root < 0 || cls < 0) { fail(why, "the options scripts were not found (a different avoptions.swf?)"); return false; }
    if (tags[cls].len < 2) { fail(why, "bad DoInitAction"); return false; }

    std::vector<Act> ra, ca;
    std::vector<uint8_t> rcode, ccode;
    if (!decode(swf.data() + tags[root].body, tags[root].len, ra, why)) return false;
    if (!decode(swf.data() + tags[cls].body + 2, tags[cls].len - 2, ca, why)) return false;
    if (!patch_root(ra, pages, cats, why) || !patch_options_menu(ca, pages.size(), why)) return false;
    if (!encode(ra, rcode, why) || !encode(ca, ccode, why)) return false;

    out.assign(swf.begin(), swf.begin() + start);
    for (int t = 0; t < (int)tags.size(); t++) {
        const Tag& g = tags[t];
        if (t != root && t != cls) { out.insert(out.end(), swf.begin() + g.hdr, swf.begin() + g.body + g.len); continue; }
        std::vector<uint8_t> body;
        if (t == cls) body.assign(swf.begin() + g.body, swf.begin() + g.body + 2);   // sprite id
        const std::vector<uint8_t>& code = t == root ? rcode : ccode;
        body.insert(body.end(), code.begin(), code.end());
        put16(out, (uint32_t)g.code << 6 | 0x3F);
        put32(out, (uint32_t)body.size());
        out.insert(out.end(), body.begin(), body.end());
    }
    if (tags.back().hdr + 2 + tags.back().len < swf.size())   // anything after the End tag
        out.insert(out.end(), swf.begin() + tags.back().body + tags.back().len, swf.end());
    const uint32_t len = (uint32_t)out.size();
    for (int i = 0; i < 4; i++) out[4 + i] = (uint8_t)(len >> (8 * i));
    return true;
}

bool patch_mainmenu(const std::vector<uint8_t>& swf, std::vector<uint8_t>& out, std::string* why)
{
    return patch_root_doaction(swf, "LOBBY_OPTION_AUDIOVISUAL", "mainmenu", patch_main_root, out, why);
}

bool patch_pausemenu(const std::vector<uint8_t>& swf, const std::vector<std::vector<PcRow>>& pages,
                     const std::vector<PcCategory>& cats, std::vector<uint8_t>& out, std::string* why)
{
    if (why) why->clear();
    if (!check_pages(pages, cats, why)) return false;
    struct PauseList { PauseList() { g_pause = true; } ~PauseList() { g_pause = false; } } pause_list;
    return patch_root_doaction(swf, "InitPauseMenu", "pausemenu",
                               [&](std::vector<Act>& a, std::string* w) { return patch_pause_root(a, pages, cats, w); }, out, why,
                               [&](std::vector<Act>& a, std::string* w) { return patch_options_menu(a, pages.size(), w); });
}

} // namespace r2swf
