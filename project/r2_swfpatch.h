// Resistance 2 PC port: ActionScript 2 bytecode patching for the "PC settings" pages of the
// audio/video options screen (r2_menurows.cpp). See r2_swfpatch.cpp.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace r2swf {

// One AVM1 action. Branches (If, Jump) and blocks (DefineFunction, DefineFunction2, With) keep
// their target as an action index (the end of the block for the latter; == the action count
// for "the end of the code"), so actions can be inserted and the offsets recomputed.
struct Act {
    uint8_t op = 0;
    std::vector<uint8_t> data;
    int target = -1;
};

bool decode(const uint8_t* code, size_t n, std::vector<Act>& out, std::string* why);
bool encode(const std::vector<Act>& acts, std::vector<uint8_t>& out, std::string* why);
// Inserts `ins` (targets relative to its own start; ins.size() = the action now at `pos`) before
// acts[pos]. Branches to `pos` from outside then run the inserted code first.
void insert(std::vector<Act>& acts, int pos, std::vector<Act> ins);

struct PcRow {
    std::string label;                  // shown with " :" appended
    std::vector<std::string> choices;   // shown in a DynamicScroll widget; the value is the index
};

// A category of the PC pages: pages [first, first + count) (0-based, in order, together all pages).
struct PcCategory {
    std::string label;   // "PC DISPLAY", ...: the category list's row and the screen title
    int first = 0;
    int count = 0;
};

// avoptions.swf (uncompressed "FWS", as the game ships it) -> the same movie with PC pages (at most 9
// rows each) in the options list, reached through a category list (at most 9 categories): the
// screen opens on that list when the native side says it came from the main menu's PC SETTINGS
// entry (FSCommand "PcStart" fills _root.pcStart = [from the entry, the entry exists]); else L1/R1
// on the AV list lead to it (only when there is no entry). On the list Enter / Right / R1 open the
// selected category's first page; on a page L1 / R1 step through the pages and Back returns to the
// list (the category still selected); Back on the list leaves (from the entry) or returns to the AV
// list. "PcGet" <page> (1-based) fills _root.optionsArray_Values (choice indices) before the list is
// rebuilt; "PcSet" "<page>:<row>:<index>" on every change; "PcHint" fills _root.pcHintC with the
// page keys' names as character codes.
bool patch_avoptions(const std::vector<uint8_t>& swf, const std::vector<std::vector<PcRow>>& pages,
                     const std::vector<PcCategory>& cats, std::vector<uint8_t>& out, std::string* why);
// The category list's rows' value text.
extern const char kCategoryOpen[];

// mainmenu.swf (the lobby's main menu) -> the same movie with a "PC SETTINGS" entry in its Options
// list (after Audio/Visual). Choosing it sends FSCommand SetScreen "PcSettings", which the native
// side turns into AVOptions opened on the PC category list (avoptions FSCommand "PcStart").
bool patch_mainmenu(const std::vector<uint8_t>& swf, std::vector<uint8_t>& out, std::string* why);

// pausemenu.swf (the in-game pause menu: campaign and co-op) -> the same movie with a "PC SETTINGS"
// entry after OPTIONS in its side list (the panel grows by a row). Choosing it opens the game's own
// options panel (OpenMenu "options": its list, font, highlight, sounds; the tab reads PC SETTINGS)
// on the same category list and pages as patch_avoptions (the pause menu's OptionsMenu class is
// patched the same way: "PcGet" / "PcSet"). Back on a page returns to the list, Back on the list
// closes the panel as the game's Back does (the game's option values are restored first).
bool patch_pausemenu(const std::vector<uint8_t>& swf, const std::vector<std::vector<PcRow>>& pages,
                     const std::vector<PcCategory>& cats, std::vector<uint8_t>& out, std::string* why);

// The actions of the movie's one root DoAction holding `marker` (tests).
bool root_actions(const std::vector<uint8_t>& swf, const char* marker, std::vector<Act>& acts, std::string* why);

} // namespace r2swf
