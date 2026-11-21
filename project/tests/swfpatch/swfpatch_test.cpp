// Offline tests for r2_swfpatch.cpp (the options screen's PC settings pages).
//
//   swfpatch_test unit             AVM1 decode / encode round trip, insert keeps branches and blocks right
//   swfpatch_test real <archive>   global_cached.psarc: patches its avoptions.swf, mainmenu.swf and pausemenu.swf
//                                  and checks the results (writes avoptions_pc.swf / mainmenu_pc.swf /
//                                  pausemenu_pc.swf; 77 = skipped)
#include "r2_swfpatch.h"
#include "mods_psarc.h"
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <crtdbg.h>
#endif
#include <map>
#include <memory>
#include <string>
#include <vector>

using Bytes = std::vector<uint8_t>;
using r2swf::Act;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static Act op(uint8_t o) { Act a; a.op = o; return a; }
static Act push_str(const char* s) { Act a; a.op = 0x96; a.data.push_back(0); a.data.insert(a.data.end(), s, s + strlen(s) + 1); return a; }
static Act branch(uint8_t o, int target) { Act a; a.op = o; a.data = { 0, 0 }; a.target = target; return a; }
static Act func(const char* name, int end)
{
    Act a; a.op = 0x9B;
    a.data.insert(a.data.end(), name, name + strlen(name) + 1);
    a.data.push_back(0); a.data.push_back(0);   // no parameters
    a.data.push_back(0); a.data.push_back(0);   // code size (from the target)
    a.target = end;
    return a;
}
static bool has(const Bytes& b, const char* s)
{
    const size_t n = strlen(s);
    for (size_t i = 0; i + n <= b.size(); i++) if (!memcmp(&b[i], s, n)) return true;
    return false;
}

/* ---- a tiny AVM1 subset, enough to run the pause menu's inserted Init code ------------------- */
struct Obj;
struct Val {
    enum K { UNDEF, NUM, STR, BOOL, OBJ } k = UNDEF;
    double n = 0; std::string s; std::shared_ptr<Obj> o;
};
struct Obj { std::map<std::string, Val> props; std::vector<Val> elems; bool array = false; };
static Val vnum(double n) { Val v; v.k = Val::NUM; v.n = n; return v; }
static Val vstr(const std::string& s) { Val v; v.k = Val::STR; v.s = s; return v; }
static Val vobj(std::shared_ptr<Obj> o) { Val v; v.k = Val::OBJ; v.o = std::move(o); return v; }
static std::string vtext(const Val& v)
{
    if (v.k == Val::STR) return v.s;
    if (v.k == Val::NUM) { char b[32]; snprintf(b, sizeof b, "%.15g", v.n); return b; }
    if (v.k == Val::BOOL) return v.n ? "true" : "false";
    return v.k == Val::UNDEF ? "undefined" : "[object]";
}
static bool vtrue(const Val& v) { return v.k == Val::NUM || v.k == Val::BOOL ? v.n != 0 : v.k == Val::STR ? !v.s.empty() : v.k == Val::OBJ; }
static Val get_member(const Val& o, const std::string& name)
{
    if (o.k != Val::OBJ) return Val();
    if (o.o->array) {
        if (name == "length") return vnum((double)o.o->elems.size());
        char* e; const long i = strtol(name.c_str(), &e, 10);
        if (!name.empty() && !*e) return i >= 0 && i < (long)o.o->elems.size() ? o.o->elems[i] : Val();
    }
    auto it = o.o->props.find(name);
    return it == o.o->props.end() ? Val() : it->second;
}
static void set_member(const Val& o, const std::string& name, const Val& v)
{
    if (o.k != Val::OBJ) return;
    char* e; const long i = strtol(name.c_str(), &e, 10);
    if (o.o->array && !name.empty() && !*e && i >= 0) { if ((size_t)i >= o.o->elems.size()) o.o->elems.resize(i + 1); o.o->elems[i] = v; }
    else o.o->props[name] = v;
}
static std::shared_ptr<Obj> arr(std::vector<Val> e) { auto o = std::make_shared<Obj>(); o->array = true; o->elems = std::move(e); return o; }

// Runs acts[begin, end) with _root = root (also register 1, variables are _root's members).
static bool avm1_run(const std::vector<Act>& acts, int begin, int end, const Val& root, std::string* why)
{
    std::vector<Val> st;
    const auto pop = [&] { Val v; if (!st.empty()) { v = st.back(); st.pop_back(); } return v; };
    int steps = 0;
    for (int pc = begin; pc != end;) {
        if (pc < begin || pc > end || ++steps > 100000) { *why = "ran away at " + std::to_string(pc); return false; }
        const Act& a = acts[pc++];
        switch (a.op) {
        case 0x96: {
            const auto& d = a.data;
            for (size_t i = 0; i < d.size();) {
                const uint8_t t = d[i++];
                if (t == 0) { std::string s((const char*)&d[i]); i += s.size() + 1; st.push_back(vstr(s)); }
                else if (t == 3) st.push_back(Val());
                else if (t == 4) { if (d[i++] != 1) { *why = "register other than r1"; return false; } st.push_back(root); }
                else if (t == 5) { Val v; v.k = Val::BOOL; v.n = d[i++]; st.push_back(v); }
                else if (t == 7) { int32_t x; memcpy(&x, &d[i], 4); i += 4; st.push_back(vnum(x)); }
                else { *why = "push type " + std::to_string(t); return false; }
            }
            break; }
        case 0x17: pop(); break;
        case 0x1C: { const std::string n = vtext(pop()); st.push_back(n == "_root" ? root : get_member(root, n)); break; }
        case 0x4E: { const std::string n = vtext(pop()); const Val o = pop(); st.push_back(get_member(o, n)); break; }
        case 0x4F: { const Val v = pop(); const std::string n = vtext(pop()); const Val o = pop(); set_member(o, n, v); break; }
        case 0x47: { const Val b = pop(), a2 = pop();
            st.push_back(a2.k == Val::STR || b.k == Val::STR ? vstr(vtext(a2) + vtext(b)) : vnum(a2.n + b.n)); break; }
        case 0x48: { const Val b = pop(), a2 = pop(); Val v; v.k = Val::BOOL; v.n = a2.n < b.n; st.push_back(v); break; }
        case 0x49: { const Val b = pop(), a2 = pop(); Val v; v.k = Val::BOOL;
            v.n = a2.k != b.k ? 0 : a2.k == Val::UNDEF ? 1 : a2.k == Val::STR ? a2.s == b.s : a2.k == Val::OBJ ? a2.o == b.o : a2.n == b.n;
            st.push_back(v); break; }
        case 0x12: { Val v; v.k = Val::BOOL; v.n = !vtrue(pop()); st.push_back(v); break; }
        case 0x50: { const Val v = pop(); st.push_back(vnum(v.n + 1)); break; }
        case 0x42: { const int n = (int)pop().n; std::vector<Val> e; for (int i = 0; i < n; i++) e.push_back(pop()); st.push_back(vobj(arr(e))); break; }
        case 0x52: {
            const std::string m = vtext(pop()); const Val o = pop(); const int n = (int)pop().n;
            std::vector<Val> args; for (int i = 0; i < n; i++) args.push_back(pop());
            if (m != "splice" || o.k != Val::OBJ || n < 2) { *why = "method " + m; return false; }
            auto& e = o.o->elems;
            const size_t at = (size_t)args[0].n;
            if (at > e.size() || args[1].n != 0) { *why = "splice args"; return false; }
            e.insert(e.begin() + at, args.begin() + 2, args.end());
            st.push_back(vobj(arr({})));
            break; }
        case 0x9D: if (vtrue(pop())) pc = a.target; break;
        case 0x99: pc = a.target; break;
        default: *why = "action 0x" + std::to_string(a.op); return false;
        }
    }
    return true;
}
// a Push's strings, ConstantPool references resolved (the movie's own code uses its pool)
static std::vector<std::string> push_strs(const std::vector<Act>& acts, const Act& a)
{
    std::vector<std::string> pool, out;
    for (const Act& c : acts)
        if (c.op == 0x88) {
            for (size_t i = 2; i < c.data.size();) { std::string s((const char*)&c.data[i]); i += s.size() + 1; pool.push_back(s); }
            break;
        }
    const auto& d = a.data;
    for (size_t i = 0; a.op == 0x96 && i < d.size();) {
        const uint8_t t = d[i++];
        if (t == 0) { std::string s((const char*)&d[i]); i += s.size() + 1; out.push_back(s); }
        else if (t == 8) { const size_t k = d[i++]; out.push_back(k < pool.size() ? pool[k] : ""); }
        else if (t == 9) { const size_t k = d[i] | d[i + 1] << 8; i += 2; out.push_back(k < pool.size() ? pool[k] : ""); }
        else if (t == 4 || t == 5) i++;
        else if (t == 1 || t == 7) i += 4;
        else if (t == 6) i += 8;
    }
    return out;
}
static int find_push(const std::vector<Act>& acts, const char* s, int from = 0)
{
    for (int i = from; i < (int)acts.size(); i++)
        for (const auto& x : push_strs(acts, acts[i])) if (x == s) return i;
    return -1;
}
/* ---- a larger AVM1 subset: the options screen's generated root code (PcOpen, PcFlip, PcBack, ...)
 * and the key handler's inserted prefix, run against stubs of the game's movie and FSCommands ---- */
struct Vm {
    struct Fn { int begin = 0, end = 0; std::vector<std::string> params; };
    const std::vector<Act>& acts;
    Val root = vobj(std::make_shared<Obj>());
    Val pad = vobj(std::make_shared<Obj>()), str = vobj(std::make_shared<Obj>());
    std::map<std::string, Fn> fns;
    std::string err;
    std::vector<std::string> sent;      // "PcGet:3", "PlaySound:CursorBack", ...
    std::string key;                    // the PadValues.Is<key>Pressed() answering true
    int from_entry = 1, has_entry = 1, inits = 0;
    // the pause menu: the list is mainMenu.optionsList; [init_b, init_e) the code inserted at the start
    // of its Init_optionsMenu (run before the game's, as in the movie); OpenMenu(section) as the game's
    bool pause = false;
    int init_b = -1, init_e = -1;
    std::vector<std::string> no_init;   // sections OpenMenu found no Init_<section>Menu for
    explicit Vm(const std::vector<Act>& a) : acts(a) {}

    Val newarr() { return vobj(arr({})); }
    void url(const std::string& u, const std::string& t)
    {
        const std::string c = u.compare(0, 10, "FSCommand:") ? u : u.substr(10);
        sent.push_back(c + ":" + t);
        if (c == "PcStart") { const Val a = get_member(root, "pcStart"); set_member(a, "0", vnum(from_entry)); set_member(a, "1", vnum(has_entry)); }
        else if (c == "PcHint") { const Val a = get_member(root, "pcHintC"); const char* k = "LB / RB";
            for (int i = 0; k[i]; i++) set_member(a, std::to_string(i), vnum(k[i]));
            set_member(a, std::to_string(strlen(k)), vnum(0)); }
        else if (c == "PcGet") set_member(get_member(root, "optionsArray_Values"), "0", vnum(1));
    }
    Val call(const std::string& name, const std::vector<Val>& args)
    {
        auto f = fns.find(name);
        if (f != fns.end()) {
            std::map<std::string, Val> loc;
            for (size_t i = 0; i < f->second.params.size(); i++) loc[f->second.params[i]] = i < args.size() ? args[i] : Val();
            bool fell = false;
            return run(f->second.begin, f->second.end, loc, fell);
        }
        if (name == "Localize") return vstr(!args.empty() && vtext(args[0]) == "LOBBY_TITLE_OPTIONS" ? "Options" : vtext(args.empty() ? Val() : args[0]));
        if (name == "Init_optionsMenu") {   // the game's: OptionsMenu.Create takes PcRows() on a PC page (patched)
            if (init_b >= 0) {
                std::map<std::string, Val> loc;
                bool fell = false;
                run(init_b, init_e, loc, fell);
                if (!fell) return Val();
            }
            inits++;
            const Val l = vobj(std::make_shared<Obj>());
            // the pause test: the list's own rows (the patched Create sets the instance's optionsArray;
            // _root.optionsArray, the game's AV rows, stays as it was)
            const bool pc = get_member(root, "pcPage").n > 0;
            if (pause) set_member(l, "rows", pc ? call("PcRows", {}) : get_member(root, "optionsArray"));
            else if (pc) set_member(root, "optionsArray", call("PcRows", {}));
            set_member(l, "currentSelection", vnum(0));
            set_member(pause ? get_member(root, "mainMenu") : get_member(get_member(root, "mainMenu"), "mc"), "optionsList", l);
            return Val();
        }
        if (name == "OpenMenu" && pause) {   // the game's: the tab takes the side list row's label, then Init_<section>Menu()
            const std::string sec = vtext(args.empty() ? Val() : args[0]);
            const Val mm = get_member(root, "mainMenu");
            set_member(root, "section", vstr("transition"));
            const Val row = get_member(get_member(root, "pauseArray"), vtext(get_member(get_member(mm, "pauseList"), "currentSelection")));
            set_member(get_member(get_member(mm, "tab"), "tab_txt"), "text", get_member(row, "0"));
            if (sec == "options") call("Init_optionsMenu", {});
            else no_init.push_back(sec);
            set_member(root, "section", vstr(sec));
            return Val();
        }
        err = "function " + name;
        return Val();
    }
    Val method(const Val& o, const std::string& m, const std::vector<Val>& args)
    {
        if (o.k == Val::STR && m == "toUpperCase") { std::string u = o.s; for (char& c : u) c = (char)toupper((unsigned char)c); return vstr(u); }
        if (m == "setTextFormat" || m == "UpdateList" || m == "UpdateSelectionBox") return Val();
        if (o.k == Val::OBJ && o.o == str.o && m == "fromCharCode") return vstr(std::string(1, (char)args.at(0).n));
        if (o.k == Val::OBJ && o.o == pad.o) { Val v; v.k = Val::BOOL; v.n = m == "Is" + key + "Pressed"; return v; }
        if (m == "ForceSelection") { set_member(o, "currentSelection", args.at(0)); return Val(); }
        err = "method " + m;
        return Val();
    }
    // acts[begin, end) with the locals loc; fell: reached end (else a Return)
    Val run(int begin, int end, std::map<std::string, Val>& loc, bool& fell)
    {
        std::vector<Val> st;
        const auto pop = [&] { Val v; if (!st.empty()) { v = st.back(); st.pop_back(); } return v; };
        const auto bval = [](bool b) { Val v; v.k = Val::BOOL; v.n = b; return v; };
        const auto num = [](const Val& v) { return v.k == Val::NUM || v.k == Val::BOOL ? v.n : v.k == Val::STR ? atof(v.s.c_str()) : NAN; };
        fell = false;
        int steps = 0;
        for (int pc = begin; err.empty();) {
            if (pc == end) { fell = true; return Val(); }
            if (pc < begin || pc > end || ++steps > 200000) { err = "ran away at " + std::to_string(pc); break; }
            const Act& a = acts[pc++];
            switch (a.op) {
            case 0x96: {
                const auto& d = a.data;
                for (size_t i = 0; i < d.size();) {
                    const uint8_t t = d[i++];
                    if (t == 0) { std::string s((const char*)&d[i]); i += s.size() + 1; st.push_back(vstr(s)); }
                    else if (t == 2 || t == 3) st.push_back(Val());
                    else if (t == 5) st.push_back(bval(d[i++] != 0));
                    else if (t == 7) { int32_t x; memcpy(&x, &d[i], 4); i += 4; st.push_back(vnum(x)); }
                    else if (t == 6) { uint32_t h, l; memcpy(&h, &d[i], 4); memcpy(&l, &d[i + 4], 4); i += 8;
                                       const uint64_t u = (uint64_t)h << 32 | l; double x; memcpy(&x, &u, 8); st.push_back(vnum(x)); }
                    else { err = "push type " + std::to_string(t); break; }
                }
                break; }
            case 0x17: pop(); break;
            case 0x1C: { const std::string n = vtext(pop());
                st.push_back(n == "_root" ? root : loc.count(n) ? loc[n] : n == "PadValues" ? pad : n == "String" ? str : get_member(root, n)); break; }
            case 0x1D: { const Val v = pop(); const std::string n = vtext(pop()); if (loc.count(n)) loc[n] = v; else set_member(root, n, v); break; }
            case 0x3C: { const Val v = pop(); loc[vtext(pop())] = v; break; }
            case 0x4E: { const std::string n = vtext(pop()); const Val o = pop(); st.push_back(get_member(o, n)); break; }
            case 0x4F: { const Val v = pop(); const std::string n = vtext(pop()); const Val o = pop(); set_member(o, n, v); break; }
            case 0x47: { const Val b = pop(), a2 = pop();
                st.push_back(a2.k == Val::STR || b.k == Val::STR ? vstr(vtext(a2) + vtext(b)) : vnum(num(a2) + num(b))); break; }
            case 0x48: { const Val b = pop(), a2 = pop(); st.push_back(bval(num(a2) < num(b))); break; }
            case 0x67: { const Val b = pop(), a2 = pop(); st.push_back(bval(num(a2) > num(b))); break; }
            case 0x49: { const Val b = pop(), a2 = pop();
                st.push_back(bval(a2.k == Val::UNDEF || b.k == Val::UNDEF ? a2.k == b.k
                                  : a2.k == Val::STR && b.k == Val::STR ? a2.s == b.s : a2.k == Val::OBJ || b.k == Val::OBJ ? a2.o == b.o
                                  : num(a2) == num(b))); break; }
            case 0x66: { const Val b = pop(), a2 = pop();
                st.push_back(bval(a2.k == b.k && (a2.k == Val::STR ? a2.s == b.s : a2.k == Val::OBJ ? a2.o == b.o : a2.n == b.n))); break; }
            case 0x44: { const Val v = pop(); st.push_back(vstr(v.k == Val::NUM ? "number" : v.k == Val::STR ? "string" : v.k == Val::BOOL ? "boolean"
                                                                : v.k == Val::UNDEF ? "undefined" : "object")); break; }
            case 0x12: st.push_back(bval(!vtrue(pop()))); break;
            case 0x50: { const Val v = pop(); st.push_back(vnum(num(v) + 1)); break; }
            case 0x42: { const int n = (int)pop().n; std::vector<Val> e; for (int i = 0; i < n; i++) e.push_back(pop()); st.push_back(vobj(arr(e))); break; }
            case 0x40: { const std::string n = vtext(pop()); const int k = (int)pop().n; for (int i = 0; i < k; i++) pop();
                if (n != "Array") { err = "new " + n; break; } st.push_back(newarr()); break; }
            case 0x3D: { const std::string n = vtext(pop()); const int k = (int)pop().n;
                std::vector<Val> args; for (int i = 0; i < k; i++) args.push_back(pop());
                st.push_back(call(n, args)); break; }
            case 0x52: { const std::string m = vtext(pop()); const Val o = pop(); const int k = (int)pop().n;
                std::vector<Val> args; for (int i = 0; i < k; i++) args.push_back(pop());
                st.push_back(method(o, m, args)); break; }
            case 0x3E: return pop();
            case 0x83: { const std::string u((const char*)a.data.data()); const std::string t((const char*)a.data.data() + u.size() + 1); url(u, t); break; }
            case 0x9A: { const std::string t = vtext(pop()), u = vtext(pop()); url(u, t); break; }
            case 0x9B: {
                Fn f;
                const auto& d = a.data;
                const std::string name((const char*)d.data());
                size_t i = name.size() + 1;
                const int np = d[i] | d[i + 1] << 8; i += 2;
                for (int k = 0; k < np; k++) { std::string q((const char*)&d[i]); i += q.size() + 1; f.params.push_back(q); }
                f.begin = pc; f.end = a.target;
                fns[name] = f;
                pc = a.target;
                break; }
            case 0x88: break;
            case 0x9D: if (vtrue(pop())) pc = a.target; break;
            case 0x99: pc = a.target; break;
            default: { char b[16]; snprintf(b, sizeof b, "0x%02X", a.op); err = std::string("action ") + b; break; }
            }
        }
        return Val();
    }
};
static std::string title_of(const Val& root, const char* f)
{
    return vtext(get_member(get_member(get_member(get_member(root, "mainMenu"), "titleBox"), f), "text"));
}
// The patched avoptions' generated root code: the category list, its pages and the way back, in the
// entry mode (from the main menu's PC SETTINGS) and without the entry (L1 / R1 on the AV list).
// pages: 4, categories "PC A" (page 1), "PC B" (pages 2, 3), "PC C" (page 4).
static void options_flow(const Bytes& av)
{
    std::vector<Act> acts;
    std::string why;
    CHECK(r2swf::root_actions(av, "Init_optionsMenu", acts, &why));
    int open_end = -1, down = -1, on_key = -1, key_end = -1;
    for (int i = 0; i < (int)acts.size(); i++)
        if (acts[i].op == 0x9B && !strcmp((const char*)acts[i].data.data(), "PcOpen")) open_end = acts[i].target;
    down = find_push(acts, "IsDownPressed");
    for (int i = down - 1; i >= 0 && down > 0; i--)
        if ((acts[i].op == 0x9B || acts[i].op == 0x8E) && acts[i].target > down) { on_key = i; break; }
    for (int i = on_key + 1; on_key >= 0 && i < (int)acts.size(); i++) if (acts[i].op == 0x9D) { key_end = acts[i].target; break; }
    CHECK(open_end > 0 && on_key > open_end && key_end > on_key);
    if (open_end <= 0 || on_key <= open_end || key_end <= on_key) return;

    const auto setup = [&](Vm& m, int from_entry, int has_entry) {
        m.from_entry = from_entry; m.has_entry = has_entry;
        const Val mm = vobj(std::make_shared<Obj>()), tb = vobj(std::make_shared<Obj>());
        const Val tt = vobj(std::make_shared<Obj>()), sub = vobj(std::make_shared<Obj>());
        set_member(tt, "text", vstr("OPTIONS : AUDIO/VISUAL :")); set_member(tt, "_x", vnum(100)); set_member(tt, "_width", vnum(300));
        set_member(tb, "title_txt", tt); set_member(tb, "subtitle_txt", sub);
        set_member(mm, "titleBox", tb); set_member(mm, "mc", vobj(std::make_shared<Obj>()));
        set_member(m.root, "mainMenu", mm);
        set_member(m.root, "section", vstr("options"));
        set_member(m.root, "optionsArray_Values", vobj(arr({ vnum(5), vnum(6) })));
        std::map<std::string, Val> loc;
        bool fell = false;
        m.run(0, open_end, loc, fell);
        CHECK(fell && m.err.empty());
        m.call("Init_optionsMenu", {});
        m.call("PcOpen", {});
    };
    // a key press: true when the inserted prefix handled it (else the game's own handler runs on)
    const auto press = [&](Vm& m, const char* k) {
        m.key = k;
        std::map<std::string, Val> loc;
        bool fell = false;
        m.run(on_key + 1, key_end, loc, fell);
        m.key.clear();
        return !fell;
    };
    const auto page = [](Vm& m) { return (int)get_member(m.root, "pcPage").n; };
    const auto sel = [](Vm& m) { return (int)get_member(get_member(get_member(get_member(m.root, "mainMenu"), "mc"), "optionsList"), "currentSelection").n; };
    const auto sent = [](Vm& m, const std::string& s) { for (const auto& x : m.sent) if (x == s) return true; return false; };

    {   // from the main menu's entry: the category list first, Back there leaves the screen
        Vm m(acts);
        setup(m, 1, 1);
        const Val rows = get_member(m.root, "optionsArray");
        CHECK(page(m) == 5 && get_member(m.root, "pcSep").n == 1);
        CHECK(get_member(rows, "length").n == 3 && vtext(get_member(get_member(rows, "1"), "0")) == "PC B :");
        CHECK(vtext(get_member(get_member(get_member(rows, "1"), "2"), "0")) == r2swf::kCategoryOpen);
        CHECK(title_of(m.root, "title_txt") == "OPTIONS : PC SETTINGS :" && title_of(m.root, "subtitle_txt") == "CHOOSE A CATEGORY");
        CHECK(get_member(get_member(m.root, "optionsArray_Values"), "length").n == 9);
        CHECK(!sent(m, "PcGet:5") && !sent(m, "PcGet:6"));
        CHECK(press(m, "Left") && page(m) == 5);            // nothing to change on the list
        CHECK(!press(m, "Down"));                            // the game moves the selection
        set_member(get_member(get_member(get_member(m.root, "mainMenu"), "mc"), "optionsList"), "currentSelection", vnum(1));
        CHECK(press(m, "Enter") && page(m) == 2 && get_member(m.root, "pcCat").n == 2 && sent(m, "PcGet:2"));
        CHECK(title_of(m.root, "title_txt") == "OPTIONS : PC B :");
        CHECK(title_of(m.root, "subtitle_txt") == "PAGE 1/2   LB / RB: PAGES   *: AT NEXT START");
        CHECK(get_member(get_member(m.root, "optionsArray_Values"), "0").n == 1);
        CHECK(press(m, "R1") && page(m) == 3 && title_of(m.root, "subtitle_txt").compare(0, 8, "PAGE 2/2") == 0);
        CHECK(press(m, "R1") && page(m) == 4 && title_of(m.root, "title_txt") == "OPTIONS : PC C :");
        CHECK(title_of(m.root, "subtitle_txt") == "LB / RB: PAGES   *: AT NEXT START");
        CHECK(press(m, "R1") && page(m) == 1);              // the pages wrap
        CHECK(press(m, "L1") && page(m) == 4);
        m.sent.clear();
        CHECK(press(m, "Back") && page(m) == 5 && sel(m) == 2 && sent(m, "PlaySound:CursorBack"));   // PC C still selected
        CHECK(title_of(m.root, "title_txt") == "OPTIONS : PC SETTINGS :");
        CHECK(press(m, "Right") && page(m) == 4);           // Right opens too
        CHECK(press(m, "Back") && page(m) == 5);
        CHECK(press(m, "L1") && page(m) == 5);              // no AV list behind it
        CHECK(!press(m, "Back") && page(m) == 5);           // the game's Back (PcReset first) leaves
        m.call("PcReset", {});
        CHECK(page(m) == 0);
        CHECK(m.err.empty()); if (!m.err.empty()) fprintf(stderr, "vm: %s\n", m.err.c_str());
    }
    {   // no entry in the main menu: L1 / R1 on Audio/Visual lead to the category list and back
        Vm m(acts);
        setup(m, 0, 0);
        const Val av = get_member(m.root, "optionsArray_Values");
        CHECK(page(m) == 0 && title_of(m.root, "subtitle_txt") == "LB / RB: PC SETTINGS");
        CHECK(press(m, "R1") && page(m) == 5 && title_of(m.root, "subtitle_txt") == "CHOOSE A CATEGORY");
        CHECK(press(m, "Enter") && page(m) == 1 && title_of(m.root, "title_txt") == "OPTIONS : PC A :");
        CHECK(press(m, "Back") && page(m) == 5 && sel(m) == 0);
        CHECK(press(m, "Back") && page(m) == 0);
        CHECK(get_member(m.root, "optionsArray_Values").o == av.o && title_of(m.root, "title_txt") == "OPTIONS : AUDIO/VISUAL :");
        CHECK(!press(m, "Back"));                            // the game's Back
        CHECK(press(m, "L1") && page(m) == 5);
        CHECK(press(m, "L1") && page(m) == 0);
        CHECK(m.err.empty()); if (!m.err.empty()) fprintf(stderr, "vm: %s\n", m.err.c_str());
    }
    {   // the main menu has the entry, the screen opened from Audio/Visual: no way to the pages there
        Vm m(acts);
        setup(m, 0, 1);
        CHECK(page(m) == 0 && title_of(m.root, "subtitle_txt").empty());
        CHECK(!press(m, "R1") && page(m) == 0);
        CHECK(!press(m, "Back"));
        CHECK(m.err.empty()); if (!m.err.empty()) fprintf(stderr, "vm: %s\n", m.err.c_str());
    }
}
// The patched pause menu's Init code (from "_root.pcI = 0" up to the panel's FrameUpdate) run on a
// campaign pause list, then again on the same movie state: one PC SETTINGS row, one +31.
static void pause_init_twice(const Bytes& po)
{
    std::vector<Act> acts;
    std::string why;
    CHECK(r2swf::root_actions(po, "InitPauseMenu", acts, &why));
    const int pci = find_push(acts, "pcI"), fu = find_push(acts, "FrameUpdate", pci < 0 ? 0 : pci);
    CHECK(pci >= 2 && fu > pci + 5);
    if (pci < 2 || fu <= pci + 5) return;
    const int begin = pci - 2, end = fu - 5;   // Push "_root"; GetVariable; Push "pcI", 0 ...  | Push r1, "gameMode" (FrameUpdate's)
    CHECK(has(acts[begin].data, "_root") && find_push(acts, "gameMode", end) == end);
    const auto row = [](const char* l, const char* s, double h) { return vobj(arr({ vstr(l), vstr(s), vnum(h) })); };
    const auto list = [&] { return vobj(arr({ row("INTEL", "intel", 31), row("DIFFICULTY", "difficulty", 31), row("CONTROLS", "controls", 31),
                                             row("OPTIONS", "options", 31), row("QUIT", "quit", 31) })); };
    Val root = vobj(std::make_shared<Obj>());
    set_member(root, "gameMode", vstr("campaign"));
    set_member(root, "campaignScale", vnum(390));
    set_member(root, "pauseArray", list());
    const auto check = [&](const char* when) {
        const Val pa = get_member(root, "pauseArray");
        int pcs = 0;
        for (int i = 0; i < (int)get_member(pa, "length").n; i++) pcs += vtext(get_member(get_member(pa, std::to_string(i)), "1")) == "pcsettings";
        const bool ok = get_member(pa, "length").n == 6 && pcs == 1
                        && vtext(get_member(get_member(pa, "4"), "0")) == "PC SETTINGS" && get_member(root, "campaignScale").n == 421;
        if (!ok) fprintf(stderr, "pause Init %s: %d rows, %d PC SETTINGS, campaignScale %s\n", when,
                         (int)get_member(pa, "length").n, pcs, vtext(get_member(root, "campaignScale")).c_str());
        CHECK(ok);
    };
    CHECK(avm1_run(acts, begin, end, root, &why)); if (!why.empty()) fprintf(stderr, "avm1: %s\n", why.c_str());
    check("once");
    CHECK(avm1_run(acts, begin, end, root, &why));
    check("twice (same movie)");
    set_member(root, "pauseArray", list());          // the game rebuilds the list, keeps the height
    CHECK(avm1_run(acts, begin, end, root, &why));
    check("new list");
    set_member(root, "campaignScale", vnum(390));    // the game resets the height, keeps the list
    CHECK(avm1_run(acts, begin, end, root, &why));
    check("height reset");
}

// The patched pause menu, as the game runs it: Enter on the side list's PC SETTINGS row (the inserted
// Enter code, then the game's OpenMenu -> Init_optionsMenu with its inserted start), the category list
// and pages in the game's own options panel, Back to the list and out, then OPTIONS as before.
// Same pages / categories as options_flow.
static void pause_flow(const Bytes& po)
{
    std::vector<Act> acts;
    std::string why;
    CHECK(r2swf::root_actions(po, "InitPauseMenu", acts, &why));
    const auto first_if_after = [&](int i) { for (int k = i; k >= 0 && k < (int)acts.size(); k++) if (acts[k].op == 0x9D) return acts[k].target; return -1; };
    int head_end = -1, init_opt = -1, open = -1;
    for (int i = 0; i < (int)acts.size(); i++) {
        if ((acts[i].op == 0x9B || acts[i].op == 0x8E) && !strcmp((const char*)acts[i].data.data(), "PcEnd")) head_end = acts[i].target;
        if ((acts[i].op == 0x9B || acts[i].op == 0x8E) && !strcmp((const char*)acts[i].data.data(), "Init_optionsMenu")) init_opt = i;
        if (push_strs(acts, acts[i]) == std::vector<std::string>{ "options", "OpenMenu" }) open = i;
    }
    int enter = -1;
    for (int i = open - 1; i >= 0 && open > 0; i--) { const auto ss = push_strs(acts, acts[i]); if (!ss.empty() && ss[0] == "pauseArray") { enter = i; break; } }
    const int down = find_push(acts, "IsDownPressed");
    int on_key = -1;
    for (int i = down - 1; i >= 0 && down > 0; i--)
        if ((acts[i].op == 0x9B || acts[i].op == 0x8E) && acts[i].target > down) { on_key = i; break; }
    const int enter_end = first_if_after(enter), key_end = first_if_after(on_key + 1), init_end = first_if_after(init_opt + 1);
    CHECK(head_end > 0 && init_opt > head_end && enter > 0 && enter_end > enter && on_key > 0 && key_end > on_key && init_end > init_opt);
    if (head_end <= 0 || init_opt <= head_end || enter <= 0 || enter_end <= enter || on_key <= 0 || key_end <= on_key || init_end <= init_opt) return;

    Vm m(acts);
    m.pause = true; m.init_b = init_opt + 1; m.init_e = init_end;
    const auto obj = [] { return vobj(std::make_shared<Obj>()); };
    const Val mm = obj(), tab = obj(), tab_txt = obj(), plist = obj();
    set_member(tab, "tab_txt", tab_txt); set_member(mm, "tab", tab);
    set_member(plist, "currentSelection", vnum(4)); set_member(mm, "pauseList", plist);
    set_member(m.root, "mainMenu", mm);
    const auto row = [](const char* l, const char* s, double h) { return vobj(arr({ vstr(l), vstr(s), vnum(h) })); };
    set_member(m.root, "pauseArray", vobj(arr({ row("INTEL", "intel", 31), row("DIFFICULTY", "difficulty", 31), row("CONTROLS", "controls", 31),
                                                row("OPTIONS", "options", 385), row("PC SETTINGS", "pcsettings", 385), row("QUIT", "quit", 31) })));
    set_member(m.root, "section", vstr("pause"));
    const Val av = vobj(arr({ vnum(5), vnum(6) })), av_rows = vobj(arr({ vstr("SFX VOLUME") }));
    set_member(m.root, "optionsArray_Values", av);
    set_member(m.root, "optionsArray", av_rows);
    {
        std::map<std::string, Val> loc;
        bool fell = false;
        m.run(0, head_end, loc, fell);
        CHECK(fell && m.err.empty());
    }
    const auto run = [&](int b, int e) { std::map<std::string, Val> loc; bool fell = false; m.run(b, e, loc, fell); return !fell; };
    // the side list's Enter: true when the inserted code took it (else the game's own Enter runs on)
    const auto side_enter = [&] { m.key = "Enter"; const bool r = run(enter, enter_end); m.key.clear(); return r; };
    const auto press = [&](const char* k) { m.key = k; const bool r = run(on_key + 1, key_end); m.key.clear(); return r; };
    const auto page = [&] { return (int)get_member(m.root, "pcPage").n; };
    const auto tab_text = [&] { return vtext(get_member(tab_txt, "text")); };
    const auto list = [&] { return get_member(mm, "optionsList"); };
    const auto sent = [&](const std::string& x) { for (const auto& y : m.sent) if (y == x) return true; return false; };

    CHECK(!press("Back") && !press("R1"));             // the side list: the game's own keys
    CHECK(side_enter());
    CHECK(m.no_init.empty());                           // the game opened its options panel, not a "pcsettings" one
    CHECK(vtext(get_member(m.root, "section")) == "options" && page() == 5 && get_member(m.root, "pcSep").n == 1);
    CHECK(tab_text() == "PC SETTINGS" && sent("PlaySound:CursorSelect") && m.inits == 1);
    const Val rows = get_member(list(), "rows");
    CHECK(get_member(rows, "length").n == 3 && vtext(get_member(get_member(rows, "2"), "0")) == "PC C :");
    for (const auto& x : m.sent) CHECK(x.find("Localize2") == std::string::npos && x.find("pcMenuReq") == std::string::npos);
    CHECK(press("Left") && page() == 5);
    CHECK(!press("Down"));                              // the game moves the selection
    set_member(list(), "currentSelection", vnum(1));
    CHECK(press("Enter") && page() == 2 && sent("PcGet:2") && tab_text() == "B 1/2");
    CHECK(get_member(get_member(m.root, "optionsArray_Values"), "0").n == 1);
    CHECK(vtext(get_member(get_member(get_member(list(), "rows"), "0"), "0")) == "SCREEN BLUR :");
    CHECK(!press("Right") && !press("Up"));             // the game changes the value / the row (UpdateOptions sends PcSet)
    CHECK(press("R1") && page() == 3 && tab_text() == "B 2/2");
    CHECK(press("R1") && page() == 4 && tab_text() == "C");
    CHECK(press("R1") && page() == 1 && tab_text() == "A");
    CHECK(press("L1") && page() == 4);
    m.sent.clear();
    CHECK(press("Back") && page() == 5 && tab_text() == "PC SETTINGS" && sent("PlaySound:CursorBack"));
    CHECK(get_member(list(), "currentSelection").n == 2);   // PC C still selected
    CHECK(!press("Back"));                              // the list: the game's own Back closes the panel
    CHECK(page() == 0 && get_member(m.root, "pcSep").n == 0 && get_member(m.root, "optionsArray_Values").o == av.o);
    // OPTIONS afterwards: the game's own rows and values
    set_member(m.root, "section", vstr("pause"));
    set_member(plist, "currentSelection", vnum(3));
    CHECK(!side_enter());
    m.call("OpenMenu", { vstr("options") });
    CHECK(page() == 0 && tab_text() == "OPTIONS" && get_member(list(), "rows").o == av_rows.o && get_member(m.root, "optionsArray_Values").o == av.o);
    CHECK(!press("R1") && !press("Back"));              // no PC keys on the game's OPTIONS
    // PC SETTINGS again: the list again, and a fresh Init (the pause menu reopened) turns it off
    set_member(m.root, "section", vstr("pause"));
    set_member(plist, "currentSelection", vnum(4));
    CHECK(side_enter() && page() == 5 && tab_text() == "PC SETTINGS");
    m.call("PcEnd", {});
    CHECK(page() == 0 && get_member(m.root, "optionsArray_Values").o == av.o && !vtrue(get_member(m.root, "pcWant")));
    CHECK(m.err.empty()); if (!m.err.empty()) fprintf(stderr, "vm: %s\n", m.err.c_str());
}

// No overlay on the in-menu paths: the pause menu's PC SETTINGS never asks for the F10 menu, and no
// F10 hint is drawn (r2_pcport.cpp / r2_menurows.cpp sources).
static void no_overlay_sources()
{
#ifdef R2_PROJECT_DIR
    const auto slurp = [](const char* name) {
        std::string path = std::string(R2_PROJECT_DIR) + "/" + name, s;
        if (FILE* f = fopen(path.c_str(), "rb")) { char b[65536]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n); fclose(f); }
        return s;
    };
    const std::string pc = slurp("r2_pcport.cpp"), mr = slurp("r2_menurows.cpp"), sp = slurp("r2_swfpatch.cpp");
    CHECK(!pc.empty() && !mr.empty() && !sp.empty());
    for (const char* bad : { "take_pcmenu", "r2_menu_rows_where", "KIND_HINT", "R2_MENU_PC_HINT", "PC menu:  F10" })
        if (pc.find(bad) != std::string::npos || mr.find(bad) != std::string::npos) { fprintf(stderr, "overlay path left: %s\n", bad); CHECK(false); }
    CHECK(sp.find("pcMenuReq") == std::string::npos && sp.find("kPauseMenuRequest") == std::string::npos);
#endif
}

static void unit()
{
    no_overlay_sources();
    // 0 Push "a"  1 If ->4  2 function f { 3 Pop }  4 Push "b"  5 Jump ->0  6 Jump ->end (7)
    std::vector<Act> acts = { push_str("a"), branch(0x9D, 4), func("f", 4), op(0x17), push_str("b"), branch(0x99, 0), branch(0x99, 7) };
    Bytes code;
    std::string why;
    CHECK(r2swf::encode(acts, code, &why));
    std::vector<Act> back;
    CHECK(r2swf::decode(code.data(), code.size(), back, &why));
    CHECK(back.size() == acts.size());
    for (size_t i = 0; i < back.size() && i < acts.size(); i++) {
        CHECK(back[i].op == acts[i].op);
        CHECK(back[i].target == acts[i].target);
    }
    Bytes again;
    CHECK(r2swf::encode(back, again, &why) && again == code);

    // two actions into the function body (before the Pop): the function grows, the branches follow
    std::vector<Act> ins = { push_str("x"), branch(0x9D, 2) };   // local target 2 = the action after the insert
    r2swf::insert(back, 3, ins);
    CHECK(back.size() == 9);
    CHECK(back[1].target == 6);          // If -> Push "b"
    CHECK(back[2].target == 6);          // function end
    CHECK(back[4].target == 5);          // inserted If -> Pop
    CHECK(back[7].target == 0);          // Jump back to the start
    CHECK(back[8].target == 9);          // Jump to the end of the code
    Bytes code2;
    CHECK(r2swf::encode(back, code2, &why));
    std::vector<Act> back2;
    CHECK(r2swf::decode(code2.data(), code2.size(), back2, &why));
    for (size_t i = 0; i < back2.size() && i < back.size(); i++) CHECK(back2[i].target == back[i].target);
    CHECK(code2.size() == code.size() + 6 + 5);   // Push "x" (3-byte header + type, x, 0) and If (3 + 2)

    // a branch into the middle of an action is refused
    Bytes bad = { 0x99, 2, 0, 0xFE, 0xFF, 0x17 };   // Jump -2: into the Jump itself
    std::vector<Act> tmp;
    CHECK(!r2swf::decode(bad.data(), bad.size(), tmp, &why));

    // not a SWF / no pages
    Bytes out;
    std::vector<std::vector<r2swf::PcRow>> one(1);
    r2swf::PcRow row; row.label = "X"; row.choices.push_back("A");
    one[0].push_back(row);
    const std::vector<r2swf::PcCategory> cat1 = { { "PC A", 0, 1 } };
    CHECK(!r2swf::patch_avoptions(Bytes{ 'C', 'W', 'S', 9 }, one, cat1, out, &why));
    CHECK(!r2swf::patch_avoptions(Bytes(64, 0), {}, cat1, out, &why));
}

static int real(const char* archive)
{
    Bytes swf;
    if (!archive || !strcmp(archive, "none") || !modpsarc::read_member(archive, "scaleform/avoptions.swf", swf)) {
        fprintf(stderr, "skipped: no global_cached.psarc\n");
        return 77;
    }
    std::vector<std::vector<r2swf::PcRow>> pages = {
        { { "FRAME RATE *", { "30 FPS", "60 FPS" } }, { "FIELD OF VIEW", { "ORIGINAL", "+5%", "+11%" } } },
        { { "SCREEN BLUR", { "ON", "OFF" } } },
        { { "CROSSHAIR", { "OFF", "DOT" } } },
        { { "MONO AUDIO", { "OFF", "ON" } } },
    };
    const std::vector<r2swf::PcCategory> cats = { { "PC A", 0, 1 }, { "PC B", 1, 2 }, { "PC C", 3, 1 } };
    Bytes out;
    std::string why;
    // the categories must cover the pages in order
    CHECK(!r2swf::patch_avoptions(swf, pages, { { "PC A", 0, 1 }, { "PC B", 1, 2 } }, out, &why));
    CHECK(!r2swf::patch_avoptions(swf, pages, { { "PC A", 0, 2 }, { "PC B", 1, 2 }, { "PC C", 3, 1 } }, out, &why));
    CHECK(!r2swf::patch_avoptions(swf, pages, {}, out, &why));
    const bool ok = r2swf::patch_avoptions(swf, pages, cats, out, &why);
    if (!ok) fprintf(stderr, "patch failed: %s\n", why.c_str());
    CHECK(ok);
    if (!ok) return 1;
    CHECK(out.size() > swf.size());
    CHECK(!memcmp(out.data(), "FWS", 3));
    CHECK((uint32_t)(out[4] | out[5] << 8 | out[6] << 16 | (uint32_t)out[7] << 24) == out.size());
    CHECK(has(out, "PcRows") && has(out, "PcFlip") && has(out, "FSCommand:PcGet") && has(out, "FSCommand:PcSet"));
    CHECK(has(out, "IsR1Pressed") && has(out, "L1 / R1: PC SETTINGS") && has(out, "FIELD OF VIEW :"));
    CHECK(has(out, "FSCommand:PcStart") && has(out, "FSCommand:PcHint") && has(out, "PcOpen") && has(out, "fromCharCode")
          && has(out, "PcBack") && has(out, "PcGo") && has(out, "CHOOSE A CATEGORY") && has(out, "PC B :"));
    // the same input gives the same bytes (the mods cache stays valid between starts)
    Bytes out2;
    CHECK(r2swf::patch_avoptions(swf, pages, cats, out2, &why) && out2 == out);
    options_flow(out);
    if (FILE* f = fopen("avoptions_pc.swf", "wb")) { fwrite(out.data(), 1, out.size(), f); fclose(f); }

    // the main menu's Options list gets a PC SETTINGS entry
    Bytes mm, mo, mo2;
    CHECK(modpsarc::read_member(archive, "scaleform/mainmenu.swf", mm));
    if (!mm.empty()) {
        const bool mok = r2swf::patch_mainmenu(mm, mo, &why);
        if (!mok) fprintf(stderr, "mainmenu patch failed: %s\n", why.c_str());
        CHECK(mok);
        CHECK(mo.size() > mm.size() && !memcmp(mo.data(), "FWS", 3));
        CHECK((uint32_t)(mo[4] | mo[5] << 8 | mo[6] << 16 | (uint32_t)mo[7] << 24) == mo.size());
        CHECK(has(mo, "PcSettings") && has(mo, "PC SETTINGS") && !has(mm, "PcSettings"));
        CHECK(r2swf::patch_mainmenu(mm, mo2, &why) && mo2 == mo);
        Bytes again;
        CHECK(!r2swf::patch_mainmenu(mo, again, &why));   // already patched: the list no longer matches
        CHECK(!r2swf::patch_mainmenu(swf, again, &why));  // not the main menu
        if (FILE* f = fopen("mainmenu_pc.swf", "wb")) { fwrite(mo.data(), 1, mo.size(), f); fclose(f); }
    }

    // the in-game pause menu gets a PC SETTINGS entry after OPTIONS (its own options panel on the PC pages)
    Bytes pm, po, po2;
    CHECK(modpsarc::read_member(archive, "scaleform/pausemenu.swf", pm));
    if (!pm.empty()) {
        const bool pok = r2swf::patch_pausemenu(pm, pages, cats, po, &why);
        if (!pok) fprintf(stderr, "pausemenu patch failed: %s\n", why.c_str());
        CHECK(pok);
    }
    if (!po.empty()) {
        CHECK(!memcmp(po.data(), "FWS", 3) || !memcmp(po.data(), "GFX", 3));   // uncompressed
        CHECK((uint32_t)(po[4] | po[5] << 8 | po[6] << 16 | (uint32_t)po[7] << 24) == po.size());
        CHECK(has(po, "pcsettings") && has(po, "PC SETTINGS") && has(po, "splice") && has(po, "pcWant") && has(po, "PcGo"));
        CHECK(has(po, "FSCommand:PcSet") && has(po, "FIELD OF VIEW :") && !has(po, "pcMenuReq"));
        CHECK(!has(pm, "pcsettings"));
        CHECK(r2swf::patch_pausemenu(pm, pages, cats, po2, &why) && po2 == po);
        Bytes again;
        CHECK(!r2swf::patch_pausemenu(po, pages, cats, again, &why));   // already patched
        CHECK(!r2swf::patch_pausemenu(mm, pages, cats, again, &why));   // not the pause menu
        CHECK(!r2swf::patch_pausemenu(pm, pages, {}, again, &why));     // the categories must cover the pages
        pause_init_twice(po);
        pause_flow(po);
        if (FILE* f = fopen("pausemenu_pc.swf", "wb")) { fwrite(po.data(), 1, po.size(), f); fclose(f); }
    }
    return 0;
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    // never a modal dialog (asserts, aborts, crashes): to stderr and the exit code
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    const char* mode = argc > 1 ? argv[1] : "unit";
    int rc = 0;
    if (!strcmp(mode, "unit")) unit();
    else if (!strcmp(mode, "real")) rc = real(argc > 2 ? argv[2] : nullptr);
    else { fprintf(stderr, "usage: swfpatch_test unit | real <global_cached.psarc>\n"); return 2; }
    if (rc == 77) return 77;
    if (g_fail || rc) { fprintf(stderr, "%d failure(s)\n", g_fail); return 1; }
    fprintf(stderr, "ok\n");
    return 0;
}
