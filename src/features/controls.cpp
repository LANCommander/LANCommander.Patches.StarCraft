// Hotkey layouts (re/notes/controls.md): the in-game Options menu gets a "Controls" section that switches the command
// card between the game's own hotkeys and a grid layout like Age of Empires II, where the keys keep the shape of
// the 3x3 card whatever it shows:
//     Q W E      positions 1 2 3
//     A S D                4 5 6
//     Z X C                7 8 9
// The choice is local (it only changes which key presses a button; the command sent is the same) and is saved
// like Game Speed (Storm registry, "Starcraft" key, value "Hotkey Layout").
//
// ---- Menus ----------------------------------------------------------------------------------------------------
// The in-game menus are modeless popups opened by FUN_004f57a0 (fastcall: ECX .bin path, EDX dialog callback),
// which destroys the popup shown before, loads the .bin, runs DialogFixup and DialogRegister (0x4F5920; ESI root,
// EAX callback). All of them use the callback 0x4CA240: init (sub-event 0) is 0x4CA160, which gives every control
// with id 1..8 the button proc 0x4CDB40 (table 0x51A404), runs per-menu set-up chosen by the activation handler in
// 0x6D1234, and increments the menu depth 0x6D1224. A button activation stores its id in 0x6D051C and the callback
// calls [0x6D1234] (0x4CA25A).
//
// Options is rez\options.bin with handler 0x4CA0A0: ids 1 Speed, 2 Sound, 3 Video, 4 Network (disabled in
// single-player by 0x4C94F0), -3 Previous. A sub-menu returns to Options with depth -= 2 and the same open call.
//
// Here, just before DialogRegister:
//   * Options: a "Controls" button (id 5, a copy of the lowest option button) goes one row below the lowest option
//     button, the controls below it move down a row, and the dialog grows by a row (its tiled background is drawn
//     to the dialog size at init, 0x4C9CC0).
//   * Controls: options.bin again, with handler ControlsActivated. The title reads "Controls", button 1 is the
//     layout toggle, button 2 cycles the HUD size (widescreen.cpp; applied at once and saved; absent when
//     widescreen is off), the rest are unlinked (never initialized) and the dialog shrinks to fit.
// The activation call 0x4CA25A goes through ActivateStub, which opens Controls for id 5 in Options.
//
// ---- Command card -----------------------------------------------------------------------------------------------
// The command card is the dialog 0x68C148 (rez\statbtn<race>.bin, FUN_00459b90). Its buttons are the children with
// ids 1..9, which are also their card positions (FUN_004591d0 gives control id N the shown button with position
// N, its record at +0x26). Its callback 0x459B00 sends char events (WM_CHAR, type 0xF) to FUN_00458b30, which
// lowercases the key and walks the children (0x417EB0) with the callback 0x4588C0 (fastcall: control, &key):
// a visible (flags 8), enabled (not 2) button with id 0..9 matches if the first character of its stat_txt string
// (record +0x10, FUN_004c36f0) is the key. The first match gets cmd 3, as a click. With the grid layout the
// callback (0x458B5A: mov ebx, 0x4588C0) is GridMatch instead, which matches letters on the position; other keys
// (Esc) keep their buttons. The tooltips show the grid key (see Tooltips below).

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/patch.h"
#include "core/patch_set.h"
#include "features/features.h"

namespace scpatch::features {
namespace {

constexpr uintptr_t kOpenPopup = 0x004F57A0;
constexpr uintptr_t kRegisterCall = 0x004F5920;  // FUN_004f57a0: call DialogRegister
constexpr uintptr_t kDialogRegister = 0x00419D20;
constexpr uintptr_t kActivateCall = 0x004CA25A;  // popup callback: call dword ptr [0x6D1234]
constexpr uintptr_t kPopupCallback = 0x004CA240;
constexpr uintptr_t kOptionsHandler = 0x004CA0A0;
constexpr uintptr_t kOptionsBin = 0x005029A8;  // "rez\options.bin"
constexpr uintptr_t kHandlerSlot = 0x006D1234;
constexpr uintptr_t kMenuDepth = 0x006D1224;
constexpr uintptr_t kActivatedId = 0x006D051C;

constexpr int16_t kControlsId = 5;  // table 0x51A404 has 8 entries, all the button proc
constexpr int16_t kToggleId = 1;
constexpr int16_t kHudSizeId = 2;
constexpr size_t kRecordSize = 0x56;

enum class Layout { Default, Grid };
Layout g_layout = Layout::Default;

const char* LayoutName(Layout layout) { return layout == Layout::Grid ? "Grid" : "Default"; }

// ---- Saved choice -----------------------------------------------------------------------------------------------

constexpr char kRegKey[] = "Starcraft";
constexpr char kRegValue[] = "Hotkey Layout";
using SRegLoadStringFn = BOOL(WINAPI*)(const char*, const char*, uint32_t, char*, uint32_t);
using SRegSaveStringFn = BOOL(WINAPI*)(const char*, const char*, uint32_t, const char*);

void LoadSaved() {
  auto load = reinterpret_cast<SRegLoadStringFn>(GetProcAddress(GetModuleHandleA("storm.dll"), MAKEINTRESOURCEA(422)));
  char saved[32] = {};
  if (!load || !load(kRegKey, kRegValue, 0, saved, sizeof(saved))) return;
  if (_stricmp(saved, "Grid") == 0) g_layout = Layout::Grid;
  if (_stricmp(saved, "Default") == 0) g_layout = Layout::Default;
}

void Save() {
  auto save = reinterpret_cast<SRegSaveStringFn>(GetProcAddress(GetModuleHandleA("storm.dll"), MAKEINTRESOURCEA(425)));
  if (!save || !save(kRegKey, kRegValue, 0, LayoutName(g_layout)))
    log::Error("controls: could not save the hotkey layout");
}

// ---- Dialog records ---------------------------------------------------------------------------------------------
// Before DialogRegister: +4/+6 left/top and +8/+A width-1/height-1 (the root's become absolute in DialogRegister;
// children stay relative to the root), +C/+E size, +14 text, +18 flags (0x200: text[0] is the lowercased hotkey),
// +20 id, +22 type, +36/+38 the root's width/height, +42 the root's children (next at +0).

template <typename T>
T& Field(uint8_t* record, int offset) {
  return *reinterpret_cast<T*>(record + offset);
}

int16_t Top(uint8_t* c) { return Field<int16_t>(c, 0x06); }

// Moves a child down by `dy`. The other rect copies (+36..+3C, +46..+4C) are moved too when they hold the same
// vertical span (they are filled at init otherwise).
void MoveChild(uint8_t* c, int dy) {
  int16_t top = Field<int16_t>(c, 0x06), bottom = Field<int16_t>(c, 0x0A);
  for (int off : {0x38, 0x48}) {
    if (Field<int16_t>(c, off) == top && Field<int16_t>(c, off + 4) == bottom) {
      Field<int16_t>(c, off) = static_cast<int16_t>(top + dy);
      Field<int16_t>(c, off + 4) = static_cast<int16_t>(bottom + dy);
    }
  }
  Field<int16_t>(c, 0x06) = static_cast<int16_t>(top + dy);
  Field<int16_t>(c, 0x0A) = static_cast<int16_t>(bottom + dy);
}

// Makes the root `dy` taller (or shorter), keeping it centred where it was.
void ResizeRoot(uint8_t* root, int dy) {
  int16_t h = Field<int16_t>(root, 0x0E), h2 = Field<int16_t>(root, 0x38);
  // +38 sizes the dialog's own buffer (DialogRegister allocates +36 * +38): it must follow the height.
  if (h2 >= h - 1 && h2 <= h + 1)
    Field<int16_t>(root, 0x38) = static_cast<int16_t>(h2 + dy);
  else
    log::Info("controls: dialog +38 is %d for height %d; left as is", h2, h);
  Field<int16_t>(root, 0x0E) = static_cast<int16_t>(h + dy);
  Field<int16_t>(root, 0x0A) = static_cast<int16_t>(Field<int16_t>(root, 0x0A) + dy);
  Field<int16_t>(root, 0x06) = static_cast<int16_t>(Field<int16_t>(root, 0x06) - dy / 2);
}

std::vector<uint8_t*> Children(uint8_t* root) {
  std::vector<uint8_t*> out;
  for (uint8_t* c = Field<uint8_t*>(root, 0x42); c; c = Field<uint8_t*>(c, 0)) out.push_back(c);
  return out;
}

uint8_t* FindId(uint8_t* root, int16_t id) {
  for (uint8_t* c : Children(root))
    if (Field<int16_t>(c, 0x20) == id) return c;
  return nullptr;
}

// The option buttons (ids 1..4) by position, and their row pitch. False if the layout is not a column of rows.
bool OptionRows(uint8_t* root, std::vector<uint8_t*>* rows, int* pitch) {
  rows->clear();
  for (int16_t id = 1; id <= 4; ++id)
    if (uint8_t* c = FindId(root, id)) rows->push_back(c);
  std::sort(rows->begin(), rows->end(), [](uint8_t* a, uint8_t* b) { return Top(a) < Top(b); });
  if (rows->size() < 2) return false;
  *pitch = Top(rows->back()) - Top((*rows)[rows->size() - 2]);
  return *pitch > 0 && *pitch < 100;
}

void LogLayout(const char* what, uint8_t* root) {
  log::Info("controls: %s dialog %d,%d %dx%d (+36 %d,%d) flags %08X", what, Field<int16_t>(root, 0x04), Top(root),
            Field<int16_t>(root, 0x0C), Field<int16_t>(root, 0x0E), Field<int16_t>(root, 0x36),
            Field<int16_t>(root, 0x38), Field<uint32_t>(root, 0x18));
  for (uint8_t* c : Children(root)) {
    const char* text = Field<const char*>(c, 0x14);
    log::Info("controls:   id %d type %d rect %d,%d,%d,%d +36 %d,%d,%d,%d flags %08X text \"%s\"", Field<int16_t>(c, 0x20),
              Field<int16_t>(c, 0x22), Field<int16_t>(c, 0x04), Top(c), Field<int16_t>(c, 0x08),
              Field<int16_t>(c, 0x0A), Field<int16_t>(c, 0x36), Field<int16_t>(c, 0x38), Field<int16_t>(c, 0x3A),
              Field<int16_t>(c, 0x3C), Field<uint32_t>(c, 0x18), text ? text : "");
  }
}

// Button text in the format of `like`: with flag 0x200 its text[0] is the hotkey (lowercased by DialogFixup and
// compared with the lowercased key by the menu matcher 0x418220), and the label follows.
void SetButtonText(uint8_t* button, uint8_t* like, char* buffer, size_t size, char hotkey, const char* label) {
  if (Field<uint32_t>(like, 0x18) & 0x200)
    _snprintf_s(buffer, size, _TRUNCATE, "%c%s", hotkey, label);
  else
    _snprintf_s(buffer, size, _TRUNCATE, "%s", label);
  Field<char*>(button, 0x14) = buffer;
}

// Replaces "Options" in the root's and the labels' texts (the menu title), keeping any leading format bytes.
void Retitle(uint8_t* root) {
  static char titles[4][64];
  int used = 0;
  std::vector<uint8_t*> all = Children(root);
  all.insert(all.begin(), root);
  for (uint8_t* c : all) {
    const char* text = Field<const char*>(c, 0x14);
    const char* at = text ? std::strstr(text, "Options") : nullptr;
    if (!at || used == 4) continue;
    char* out = titles[used++];
    _snprintf_s(out, sizeof(titles[0]), _TRUNCATE, "%.*sControls%s", static_cast<int>(at - text), text,
                at + std::strlen("Options"));
    Field<char*>(c, 0x14) = out;
  }
  if (!used) log::Info("controls: no \"Options\" title found to rename");
}

alignas(4) uint8_t g_controlsButton[(kRecordSize + 3) & ~size_t{3}];
char g_controlsText[32];
char g_toggleText[48];
char g_hudSizeText[48];

void ExtendOptions(uint8_t* root) {
  static bool logged;
  if (!logged) LogLayout("options", root);
  logged = true;
  std::vector<uint8_t*> rows;
  int pitch;
  if (!OptionRows(root, &rows, &pitch) || FindId(root, kControlsId)) {
    log::Error("controls: options menu layout not as expected; no Controls button");
    return;
  }
  uint8_t* last = rows.back();
  int16_t lastTop = Top(last);
  for (uint8_t* c : Children(root))
    if (Top(c) > lastTop) MoveChild(c, pitch);
  std::memcpy(g_controlsButton, last, kRecordSize);
  Field<int16_t>(g_controlsButton, 0x20) = kControlsId;
  MoveChild(g_controlsButton, pitch);
  SetButtonText(g_controlsButton, last, g_controlsText, sizeof(g_controlsText), 'c', "Controls");
  Field<uint8_t*>(g_controlsButton, 0) = Field<uint8_t*>(last, 0);
  Field<uint8_t*>(last, 0) = g_controlsButton;
  ResizeRoot(root, pitch);
}

void ShapeControls(uint8_t* root) {
  std::vector<uint8_t*> rows;
  int pitch;
  if (!OptionRows(root, &rows, &pitch) || Field<int16_t>(rows.front(), 0x20) != kToggleId) {
    log::Error("controls: options menu layout not as expected; Controls menu left as is");
    return;
  }
  uint8_t* toggle = rows.front();
  char label[40];
  _snprintf_s(label, _TRUNCATE, "Hotkeys: %s", LayoutName(g_layout));
  SetButtonText(toggle, toggle, g_toggleText, sizeof(g_toggleText), 'h', label);
  size_t kept = 1;
  if (HudSizeAvailable() && Field<int16_t>(rows[1], 0x20) == kHudSizeId) {
    _snprintf_s(label, _TRUNCATE, "HUD Size: %d%%", HudSizeChosen());
    SetButtonText(rows[1], rows[1], g_hudSizeText, sizeof(g_hudSizeText), 's', label);
    kept = 2;
  }
  Retitle(root);
  // Unlink the other option rows, then close the gap they leave.
  std::vector<int16_t> removedTops;
  for (size_t i = kept; i < rows.size(); ++i) {
    removedTops.push_back(Top(rows[i]));
    for (uint8_t** link = &Field<uint8_t*>(root, 0x42); *link; link = &Field<uint8_t*>(*link, 0)) {
      if (*link == rows[i]) {
        *link = Field<uint8_t*>(rows[i], 0);
        break;
      }
    }
  }
  for (uint8_t* c : Children(root)) {
    int above = static_cast<int>(std::count_if(removedTops.begin(), removedTops.end(), [&](int16_t t) { return t < Top(c); }));
    if (above) MoveChild(c, -pitch * above);
  }
  ResizeRoot(root, -pitch * static_cast<int>(removedTops.size()));
}

void OpenPopup(uintptr_t handler) {
  *reinterpret_cast<uintptr_t*>(kHandlerSlot) = handler;
  reinterpret_cast<void(__fastcall*)(uintptr_t, uintptr_t)>(kOpenPopup)(kOptionsBin, kPopupCallback);
}

// The Controls menu's activation handler (0x6D1234).
void __cdecl ControlsActivated() {
  auto& depth = *reinterpret_cast<uint8_t*>(kMenuDepth);
  int32_t id = *reinterpret_cast<int32_t*>(kActivatedId);
  if (id == kToggleId || (id == kHudSizeId && HudSizeAvailable())) {
    if (id == kToggleId) {
      g_layout = g_layout == Layout::Grid ? Layout::Default : Layout::Grid;
      Save();
      log::Info("controls: hotkey layout %s", LayoutName(g_layout));
    } else {
      HudSizeChoose(HudSizeNext(HudSizeChosen()));  // applied before the next frame
      log::Info("controls: HUD size %d%% chosen", HudSizeChosen());
    }
    if (depth) --depth;  // reopened: its init counts it again
    OpenPopup(reinterpret_cast<uintptr_t>(&ControlsActivated));
    return;
  }
  depth = static_cast<uint8_t>(depth >= 2 ? depth - 2 : 0);  // back to Options, as the other sub-menus return
  OpenPopup(kOptionsHandler);
}

void __cdecl BeforeRegister(uint8_t* root, uintptr_t callback) {
  if (!root || callback != kPopupCallback) return;
  uintptr_t handler = *reinterpret_cast<uintptr_t*>(kHandlerSlot);
  if (handler == kOptionsHandler) ExtendOptions(root);
  if (handler == reinterpret_cast<uintptr_t>(&ControlsActivated)) ShapeControls(root);
}

uintptr_t g_dialogRegister = kDialogRegister;
uintptr_t g_handlerSlot = kHandlerSlot, g_activatedId = kActivatedId;

// Replaces the call to DialogRegister (ESI root, EAX callback).
__declspec(naked) void RegisterStub() {
  __asm {
    pushad
    push eax
    push esi
    call BeforeRegister
    add esp, 8
    popad
    jmp dword ptr [g_dialogRegister]
  }
}

void __cdecl OpenControls() { OpenPopup(reinterpret_cast<uintptr_t>(&ControlsActivated)); }

// Replaces `call dword ptr [0x6D1234]` (6 bytes: call rel32 + nop). Registers are as the handler expects.
__declspec(naked) void ActivateStub() {
  __asm {
    push eax
    push ecx
    mov ecx, g_handlerSlot
    mov eax, [ecx]
    cmp eax, 0x004CA0A0  // kOptionsHandler
    jne original
    mov ecx, g_activatedId
    cmp dword ptr [ecx], 5  // kControlsId
    jne original
    pop ecx
    pop eax
    pushad
    call OpenControls
    popad
    ret
  original:
    pop ecx
    pop eax
    push edx
    mov edx, g_handlerSlot
    mov edx, [edx]
    xchg edx, [esp]  // the handler's address on the stack, EDX restored
    ret              // tail-jumps to it; it returns to our caller
  }
}

// ---- Command card hotkeys ---------------------------------------------------------------------------------------

constexpr uintptr_t kCardCallbackSet = 0x00458B5A;  // FUN_00458b30: mov ebx, 0x4588C0
constexpr uintptr_t kCardMatch = 0x004588C0;
constexpr char kGridKeys[] = "qweasdzxc";  // positions 1..9

bool IsLetter(uint8_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// Same conditions as 0x4588C0, matched on the position instead of the string's first character. Keys that are
// not letters (Esc for Cancel) keep their own buttons.
int __fastcall GridMatch(uint8_t* control, const uint8_t* key) {
  if (g_layout != Layout::Grid || !IsLetter(*key))
    return reinterpret_cast<int(__fastcall*)(uint8_t*, const uint8_t*)>(kCardMatch)(control, key);
  int16_t id = Field<int16_t>(control, 0x20);
  uint32_t flags = Field<uint32_t>(control, 0x18);
  if (id < 1 || id > 9 || !(flags & 8) || (flags & 2) || !Field<void*>(control, 0x26)) return 0;
  return *key == static_cast<uint8_t>(kGridKeys[id - 1]) ? 1 : 0;
}

// ---- Tooltips ---------------------------------------------------------------------------------------------------
// FUN_004593a0 (EDI the hovered button) looks up the button's string at 0x459411 (ESI the button record, whose +0
// is its position; CX the string id; the string comes back in EAX and EDX must survive). An enabled button's
// tooltip is drawn by FUN_00459150 (ECX = string + 2: after the hotkey and the button-type byte) at 0x4595A8 and
// 0x459644; it renders the text at once (FUN_00459030), keeping no pointer. With the grid layout the text drawn
// there loses the highlight of a letter hotkey (the first 0x03 colour byte and the colour byte that ends it) and
// gets the grid key, highlighted the same way, at the end of its first line: "Train Marine (Q)".

constexpr uintptr_t kTipLookupCall = 0x00459411;
constexpr uintptr_t kStringLookup = 0x004C36F0;
constexpr uintptr_t kTipDrawCalls[] = {0x004595A8, 0x00459644};
constexpr uintptr_t kTipDraw = 0x00459150;
constexpr uint8_t kHighlight = 0x03;

const char* g_tipString;  // the hovered button's string, from the lookup
int g_tipPosition;
char g_tipText[128];      // FUN_00459150 formats into 0x80 bytes

void __cdecl NoteTooltip(const char* string, const uint8_t* record) {
  g_tipString = string;
  g_tipPosition = record ? *reinterpret_cast<const int16_t*>(record) : 0;
}

bool IsColour(uint8_t c) { return c && c < 0x20 && c != '\n' && c != '\r' && c != '\t'; }

const char* __cdecl TooltipText(const char* text) {
  if (g_layout != Layout::Grid || !g_tipString || text != g_tipString + 2 || g_tipPosition < 1 || g_tipPosition > 9)
    return text;
  std::string out;
  char restore = 0x01;  // the colour after the highlight
  bool strip = IsLetter(static_cast<uint8_t>(g_tipString[0]));  // the old letter no longer works; Esc still does
  bool inHighlight = false, stripped = false;
  for (const char* p = text; *p; ++p) {
    uint8_t c = static_cast<uint8_t>(*p);
    if (strip && !stripped && !inHighlight && c == kHighlight) {
      inHighlight = true;
      continue;
    }
    if (inHighlight && IsColour(c)) {
      restore = static_cast<char>(c);
      inHighlight = false;
      stripped = true;
      continue;
    }
    out += static_cast<char>(c);
  }
  size_t line = out.find('\n');
  if (line == std::string::npos) line = out.size();
  char key[8] = {' ', '(', static_cast<char>(kHighlight), static_cast<char>(kGridKeys[g_tipPosition - 1] - 'a' + 'A'),
                 restore, ')', 0};
  out.insert(line, key);
  _snprintf_s(g_tipText, _TRUNCATE, "%s", out.c_str());
  static int logged;
  if (logged < 3) {
    ++logged;
    std::string escaped;
    for (const char* p = text; *p; ++p) {
      char hex[8];
      if (static_cast<uint8_t>(*p) < 0x20) {
        _snprintf_s(hex, _TRUNCATE, "<%02X>", static_cast<uint8_t>(*p));
        escaped += hex;
      } else {
        escaped += *p;
      }
    }
    log::Info("controls: tooltip at position %d: \"%s\" -> grid key %c", g_tipPosition, escaped.c_str(),
              kGridKeys[g_tipPosition - 1]);
  }
  return g_tipText;
}

uintptr_t g_stringLookup = kStringLookup, g_tipDraw = kTipDraw;

// Replaces the string lookup call at 0x459411; ESI is the button record. Keeps EAX (the string) and EDX.
__declspec(naked) void TipLookupStub() {
  __asm {
    call dword ptr [g_stringLookup]
    push eax
    push ecx
    push edx
    push esi
    push eax
    call NoteTooltip
    add esp, 8
    pop edx
    pop ecx
    pop eax
    ret
  }
}

// Replaces the tooltip draw calls: ECX = TooltipText(ECX), the rest as it was.
__declspec(naked) void TipDrawStub() {
  __asm {
    push eax
    push edx
    push ecx
    call TooltipText
    add esp, 4
    mov ecx, eax
    pop edx
    pop eax
    jmp dword ptr [g_tipDraw]
  }
}

void InstallTooltips() {
  bool ok = patch::RedirectCall(kTipLookupCall, kStringLookup, reinterpret_cast<void*>(&TipLookupStub),
                                "controls: tooltip string");
  for (uintptr_t call : kTipDrawCalls)
    ok = ok && patch::RedirectCall(call, kTipDraw, reinterpret_cast<void*>(&TipDrawStub), "controls: tooltip draw");
  if (!ok) log::Error("controls: tooltips keep the default hotkeys");
}

patch::PatchSet g_menu("controls menu");
patch::PatchSet g_card("controls card");

}  // namespace

void InstallControls() {
  std::wstring initial = config::GetString(L"Controls", L"Hotkeys", L"Default");
  g_layout = _wcsicmp(initial.c_str(), L"Grid") == 0 ? Layout::Grid : Layout::Default;
  LoadSaved();  // the menu's choice, once made, wins
  uint32_t match = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&GridMatch));
  g_card.Add(kCardCallbackSet, {0xBB, 0xC0, 0x88, 0x45, 0x00},
             {0xBB, uint8_t(match), uint8_t(match >> 8), uint8_t(match >> 16), uint8_t(match >> 24)},
             "command card hotkey match");
  if (!g_card.Verify()) {
    log::Error("controls: command card not patched; hotkey layouts unavailable");
    return;
  }
  g_card.Enable(true);
  InstallTooltips();
  uint32_t reg = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&RegisterStub) - (kRegisterCall + 5));
  uint32_t act = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&ActivateStub) - (kActivateCall + 5));
  g_menu.Add(kRegisterCall, {0xE8, 0xFB, 0x43, 0xF2, 0xFF},
             {0xE8, uint8_t(reg), uint8_t(reg >> 8), uint8_t(reg >> 16), uint8_t(reg >> 24)}, "popup register");
  g_menu.Add(kActivateCall, {0xFF, 0x15, 0x34, 0x12, 0x6D, 0x00},
             {0xE8, uint8_t(act), uint8_t(act >> 8), uint8_t(act >> 16), uint8_t(act >> 24), 0x90}, "popup activate");
  if (!g_menu.Verify()) {
    log::Error("controls: menu not installed");
    return;
  }
  g_menu.Enable(true);
  log::Info("controls: Options > Controls installed; hotkey layout %s", LayoutName(g_layout));
}

}  // namespace scpatch::features
