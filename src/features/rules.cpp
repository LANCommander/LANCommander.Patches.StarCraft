// Lobby game rules (re/notes/lobby.md): gameplay options the host chooses that every player must share.
//
// Rules travel as Storm lobby messages (SNetSendMessage, ordinal 127) with a first byte the game does not use, so
// unpatched players drop them in LobbyMessageDispatch's default case. Patched players take them out of
// SNetReceiveMessage (ordinal 121, wrapped through the EXE's import) before the game sees them.
//
// Lobby commands, including "start game" (0x3C), travel in the turn stream instead, which is not ordered with
// these messages. So the host versions what it sends and holds the Start button until every patched player has
// confirmed the latest version:
//   * Host: on every join or leave it works out the rules. Its own choice applies only if every other player has
//     answered (is patched); otherwise vanilla rules apply. When the result changes it sends RULES(seq, rules) to
//     each player, and re-sends every second to players that have not confirmed `seq`.
//   * Joiner: starts each lobby at vanilla rules, adopts RULES from the host (Storm player 0) if it can play them,
//     and always answers ACK(seq, the most it can play). The host's choice applies only if every player can play it.
//   * Start (FUN_004b9910, Start button): refused with a message while the rules for the players now in the lobby
//     differ from the last ones sent, or a patched player has not confirmed them. Otherwise they are frozen.
// An unpatched player never blocks the start; it just makes the game vanilla. A patched player that has not
// answered yet still holds vanilla rules, which is what the host uses then.
//
// Replays: the rules a game was played with are appended to its replay file as a trailer ("SCPR", version, size,
// rules) when the writer FUN_004dfab0 closes it. The game's reader FUN_004df570 reads fixed sections and closes
// the file, so vanilla never sees the trailer; here it is read just before that close. No trailer: vanilla rules.

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <mutex>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "core/patch_set.h"
#include "features/features.h"

namespace scpatch::features {
namespace {

constexpr uintptr_t kOwnPlayer = 0x0051268C;   // our Storm player id, written by the create/join calls
constexpr uintptr_t kCreateCall = 0x004D3B0B;  // call SNetCreateGame (ordinal 138 thunk), 12 args
constexpr uintptr_t kCreateThunk = 0x00410118;
constexpr uintptr_t kJoinCall = 0x004D409E;  // call SNetJoinGame (ordinal 101 thunk), 10 args
constexpr uintptr_t kJoinThunk = 0x0041010C;
constexpr uintptr_t kLeftHandlerPush = 0x004D3F1A;    // push FUN_004c4980 (event 2: player left)
constexpr uintptr_t kJoinedHandlerPush = 0x004D3F2A;  // push FUN_004c3c90 (event 3: player joined)
constexpr uintptr_t kLeftHandler = 0x004C4980;
constexpr uintptr_t kJoinedHandler = 0x004C3C90;
constexpr uintptr_t kStartCall = 0x004B99CD;  // Start button: call FUN_00452460 (queue "start game")
constexpr uintptr_t kStartRequest = 0x00452460;
constexpr uintptr_t kStartContinue = 0x004B99D2;
constexpr uintptr_t kShowMessage = 0x004B7180;  // lobby message box, text in EAX (as after FUN_004ddd30)

constexpr uintptr_t kReplayWriteClose = 0x004DFC49;  // FUN_004dfab0: call fclose
constexpr uintptr_t kReplayReadClose = 0x004DF6B1;   // FUN_004df570: call fclose (end of reading, or failure)
constexpr uintptr_t kFclose = 0x0040D483;            // the EXE's CRT (the FILE* belongs to it)
constexpr uintptr_t kFread = 0x004117DE;
constexpr uintptr_t kFwrite = 0x00411931;

constexpr WORD kOrdReceiveMessage = 121;
constexpr WORD kOrdSendMessage = 127;
constexpr int kMaxPlayers = 8;  // the game ignores Storm player ids >= 8
constexpr DWORD kResendMs = 1000;

constexpr int kVanillaLimit = 12;

// Message: id, magic, type, seq (u32), then the rules.
constexpr uint8_t kMessageId = 0xA7;
constexpr uint8_t kMagic[3] = {'S', 'C', 'P'};
constexpr uint8_t kTypeRules = 1;
constexpr uint8_t kTypeAck = 2;

#pragma pack(push, 1)
// Fields are only ever appended: a peer or replay with fewer bytes leaves the rest at their (vanilla) defaults.
struct Rules {
  uint8_t selectionLimit = kVanillaLimit;
  uint8_t zoom = 0;  // 1: zooming allowed (vanilla has none)
  uint8_t rallyMining = 0;  // 1: workers rallied to a resource harvest it
  bool operator==(const Rules& o) const {
    return selectionLimit == o.selectionLimit && zoom == o.zoom && rallyMining == o.rallyMining;
  }
};
struct Message {
  uint8_t id = kMessageId;
  uint8_t magic[3] = {kMagic[0], kMagic[1], kMagic[2]};
  uint8_t type = 0;
  uint32_t seq = 0;
  Rules rules;
};
#pragma pack(pop)

struct StormEvent {
  uint32_t flags;
  uint32_t player;
  void* data;
  uint32_t size;
};

using ReceiveFn = BOOL(WINAPI*)(uint32_t*, const uint8_t**, uint32_t*);
using SendFn = BOOL(WINAPI*)(uint32_t, const void*, uint32_t);
using HandlerFn = void(WINAPI*)(StormEvent*);
using CreateFn = BOOL(WINAPI*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                               uint32_t, uint32_t, uint32_t, uint32_t);
using JoinFn = BOOL(WINAPI*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                             uint32_t, uint32_t);

ReceiveFn g_receive;
SendFn g_send;

enum class Role { None, Host, Joiner };

struct Peer {
  bool present = false;
  bool patched = false;  // has answered at least once
  Rules capable;         // the most it can play (from its answers)
  uint32_t acked = 0;    // last seq it confirmed
  DWORD sentAt = 0;
};

// Lobby state. Storm calls the event handlers from inside its own calls on the game thread, but nothing here
// depends on that: everything is under the lock, and messages are sent outside it.
std::mutex g_lock;
Role g_role = Role::None;
bool g_frozen;        // host: Start accepted, rules final
Rules g_wanted;       // host: what the host asks for
Rules g_current;      // host: last rules sent; joiner: last rules received
uint32_t g_seq;       // host: version of g_current
Peer g_peers[kMaxPlayers];
bool g_zoomChoice = true;  // the host's "Zooming" choice on the create screen

Rules WantedRules() {
  Rules r;
  r.selectionLimit = static_cast<uint8_t>(SelectionPreferredLimit());
  r.zoom = g_zoomChoice ? 1 : 0;
  r.rallyMining = RallyMiningPreferred() ? 1 : 0;
  return r;
}

// The most this client can play.
Rules Capability() {
  Rules r;
  r.selectionLimit = static_cast<uint8_t>(SelectionAvailable() ? 24 : kVanillaLimit);
  r.zoom = 1;  // only changes what a player may see; a client without zoom just does not zoom
  r.rallyMining = RallyMiningAvailable() ? 1 : 0;
  return r;
}

// `rules` fits within `capable`. Zooming does not affect the simulation, so any patched player can play it.
bool Fits(const Rules& rules, const Rules& capable) {
  return (rules.selectionLimit == kVanillaLimit || rules.selectionLimit == capable.selectionLimit) &&
         (!rules.rallyMining || capable.rallyMining);
}

const char* OnOff(uint8_t zoom) { return zoom ? "enabled" : "disabled"; }

// Host: the rules for the players now in the lobby. Under the lock.
Rules HostRules(bool* everyonePatched) {
  // Rally mining is dropped alone when a patched player cannot play it (an older scpatch).
  Rules wanted = g_wanted;
  for (const Peer& p : g_peers)
    if (p.present && p.patched && !p.capable.rallyMining) wanted.rallyMining = 0;
  bool ok = true;
  for (const Peer& p : g_peers) ok &= !p.present || (p.patched && Fits(wanted, p.capable));
  if (everyonePatched) *everyonePatched = ok;
  return ok ? wanted : Rules{};
}

void NewSession(Role role) {
  std::lock_guard<std::mutex> hold(g_lock);
  g_role = role;
  g_frozen = false;
  g_wanted = role == Role::Host ? WantedRules() : Rules{};
  g_current = Rules{};
  g_seq = 0;
  for (Peer& p : g_peers) p = Peer{};
  log::Info("rules: %s a game (own player %u); asking for selection limit %d, zooming %s, rally mining %s",
            role == Role::Host ? "hosting" : "joined", *reinterpret_cast<uint32_t*>(kOwnPlayer), g_wanted.selectionLimit,
            OnOff(g_wanted.zoom), OnOff(g_wanted.rallyMining));
}

struct Outgoing {
  uint32_t to;
  Message message;
};

// Host: recompute the rules and collect what must be (re)sent. Under the lock.
void HostPump(std::vector<Outgoing>& out) {
  if (g_role != Role::Host || g_frozen) return;
  bool allPatched;
  Rules rules = HostRules(&allPatched);
  if (!(rules == g_current) || g_seq == 0) {
    g_current = rules;
    ++g_seq;
    for (Peer& p : g_peers) p.sentAt = 0;
    log::Info("rules: version %u, selection limit %d, zooming %s, rally mining %s%s", g_seq, rules.selectionLimit,
              OnOff(rules.zoom), OnOff(rules.rallyMining),
              allPatched ? "" : " (a player without scpatch, or unable to play the host's rules, is in the lobby)");
  }
  DWORD now = GetTickCount();
  for (uint32_t id = 0; id < kMaxPlayers; ++id) {
    Peer& p = g_peers[id];
    if (!p.present || p.acked == g_seq || (p.sentAt && now - p.sentAt < kResendMs)) continue;
    p.sentAt = now;
    Message m;
    m.type = kTypeRules;
    m.seq = g_seq;
    m.rules = g_current;
    out.push_back({id, m});
  }
}

void Send(const std::vector<Outgoing>& out) {
  for (const Outgoing& o : out) g_send(o.to, &o.message, sizeof(o.message));
}

bool IsOurs(const uint8_t* data, uint32_t length) {
  return data && length >= 5 && data[0] == kMessageId && std::memcmp(data + 1, kMagic, 3) == 0;
}

void Handle(uint32_t from, const uint8_t* data, uint32_t length, std::vector<Outgoing>& out) {
  // A newer peer may send more rules, an older one fewer: read what both know, defaults for the rest.
  if (length < offsetof(Message, rules) + 1) return;
  Message m;
  std::memcpy(&m, data, std::min<size_t>(length, sizeof(m)));
  std::lock_guard<std::mutex> hold(g_lock);
  if (m.type == kTypeRules && g_role == Role::Joiner && from == 0) {
    // The host only offers rules every player can play; keep vanilla if this one somehow cannot.
    Rules adopted = Fits(m.rules, Capability()) ? m.rules : Rules{};
    if (!(adopted == g_current))
      log::Info("rules: host set selection limit %d, zooming %s, rally mining %s", adopted.selectionLimit,
                OnOff(adopted.zoom), OnOff(adopted.rallyMining));
    g_current = adopted;
    Message ack;
    ack.type = kTypeAck;
    ack.seq = m.seq;
    ack.rules = Capability();
    out.push_back({from, ack});
  } else if (m.type == kTypeAck && g_role == Role::Host && from < kMaxPlayers && g_peers[from].present) {
    Peer& p = g_peers[from];
    if (!p.patched) log::Info("rules: player %u has scpatch (selection limit up to %d)", from, m.rules.selectionLimit);
    p.patched = true;
    p.capable = m.rules;
    if (m.seq <= g_seq) p.acked = m.seq;
  }
}

// SNetReceiveMessage: our messages are handled and skipped; also the place where the host sends.
BOOL WINAPI ReceiveMessage(uint32_t* from, const uint8_t** data, uint32_t* length) {
  for (;;) {
    BOOL got = g_receive(from, data, length);
    std::vector<Outgoing> out;
    if (got && IsOurs(*data, *length)) Handle(*from, *data, *length, out);
    {
      std::lock_guard<std::mutex> hold(g_lock);
      HostPump(out);
    }
    Send(out);
    if (!got || !IsOurs(*data, *length)) return got;
  }
}

void WINAPI PlayerJoined(StormEvent* e) {
  {
    std::lock_guard<std::mutex> hold(g_lock);
    if (g_role == Role::Host && !g_frozen && e->player < kMaxPlayers &&
        e->player != *reinterpret_cast<uint32_t*>(kOwnPlayer)) {
      g_peers[e->player] = Peer{};
      g_peers[e->player].present = true;
    }
  }
  reinterpret_cast<HandlerFn>(kJoinedHandler)(e);
}

void WINAPI PlayerLeft(StormEvent* e) {
  {
    std::lock_guard<std::mutex> hold(g_lock);
    if (e->player < kMaxPlayers && !g_frozen) g_peers[e->player] = Peer{};
  }
  reinterpret_cast<HandlerFn>(kLeftHandler)(e);
}

BOOL WINAPI CreateGame(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7,
                       uint32_t a8, uint32_t a9, uint32_t a10, uint32_t a11, uint32_t a12) {
  BOOL ok = reinterpret_cast<CreateFn>(kCreateThunk)(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12);
  if (ok) NewSession(Role::Host);
  return ok;
}

BOOL WINAPI JoinGame(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7,
                     uint32_t a8, uint32_t a9, uint32_t a10) {
  NewSession(Role::Joiner);  // before the call: the host's first message may arrive as soon as we are in
  return reinterpret_cast<JoinFn>(kJoinThunk)(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
}

// Start button: nullptr to let the start through (rules frozen), or the message explaining why not.
const char* CheckStart() {
  std::lock_guard<std::mutex> hold(g_lock);
  if (g_role != Role::Host || g_frozen) return nullptr;
  constexpr const char* kWait = "Game rules are still being sent to the other players. Try again in a moment.";
  if (!(HostRules(nullptr) == g_current)) {
    log::Info("rules: start held, the players changed since version %u was sent", g_seq);
    return kWait;  // the next receive call sends the new version
  }
  for (uint32_t id = 0; id < kMaxPlayers; ++id) {
    const Peer& p = g_peers[id];
    if (p.present && p.patched && p.acked != g_seq) {
      log::Info("rules: start held, player %u has not confirmed version %u", id, g_seq);
      return kWait;
    }
  }
  g_frozen = true;
  log::Info("rules: game starting with selection limit %d, zooming %s, rally mining %s (version %u)",
            g_current.selectionLimit, OnOff(g_current.zoom), OnOff(g_current.rallyMining), g_seq);
  return nullptr;
}

uint32_t g_startRequest = kStartRequest, g_startContinue = kStartContinue, g_showMessage = kShowMessage;

// Replaces `call FUN_00452460` in the Start button case. EDI still holds the dialog, as the vanilla refusals expect.
__declspec(naked) void StartStub() {
  __asm {
    call CheckStart
    test eax, eax
    jnz blocked
    call dword ptr [g_startRequest]
    jmp dword ptr [g_startContinue]
  blocked:
    call dword ptr [g_showMessage]
    pop edi
    pop esi
    mov eax, 1
    pop ebx
    ret
  }
}

// ---- Create-game screen -------------------------------------------------------------------------------------
// FUN_004af6d0 loads the screen (rez\gluCreat.bin in multiplayer) and runs it modally with FUN_004af5c0 as the
// dialog callback. Control records (0x56 bytes, children of the root at +0x42, next at +0x00): +4/+6/+8/+A rect
// (left, top, right, bottom), +C/+E size, +14 text, +18 flags (bit 0 dirty, bit 3 shown), +20 id, +22 type,
// +2A event proc, +2E draw proc, +32 parent, +36..+3C hit rect (relative until the control is initialized),
// +46..+4C rect copy. Controls are initialized inside the modal loop, so a record linked in before it is set up
// like the loaded ones.
//
// In multiplayer, rows for the rules go under Game Type / Teams / Speed in the left panel ("Selection Limit" when
// this client can play 24 units, then "Zooming:", then "Rally Mining:"): each a label cloned from the "Game Type" label and a dropdown
// cloned from the Game Type dropdown (id 0x11, type 13). (Single-player custom games show the player-slots panel
// there; [Selection] Limit applies, and zooming is always allowed.)
//
// The panel is an image control (id 1, type 5, pListMap.pcx): +C/+E/+10 is a ScreenDesc {w, h, pixels} (363x332)
// that FUN_004ef560 blits. The panel is made 22 px taller per row: rows 0..324, then a copy of the Teams row band
// (283..304) for each new row from 325, then the bottom border piece (325..331; splitting at 328, as first
// measured on a screenshot, cut the border's top 3 px off and left them above the new rows). The pixels are a Storm allocation (freed on destroy by FUN_00418c40), so the taller copy is one too.
//
// Dropdowns (dlg\lbox.cpp): +3A item text array, +3E item flags, +42 item data, +46 count, +47 capacity,
// +48 selected index (0xFF none), +49 item height. The arrays are allocated when the control is initialized, in
// the modal loop (the raw records cloned here have none). Items are added with FUN_0041b760 (ECX dropdown,
// EAX text, push flags (0x10: copy the text); returns the index in AL) and selected by text with FUN_0041a890
// (stdcall dropdown, text). The game fills its own dropdowns on the dialog's init event (type 0xE, sub-event 0)
// and so does the wrapper, after it. The choice is read when OK is activated (sub-event 2, DAT_006d051c == 12),
// before the game is created.

constexpr uintptr_t kCreateModalCall = 0x004AF763;  // call FUN_0041a080 (modal loop), EAX = dialog root
constexpr uintptr_t kModalLoop = 0x0041A080;
constexpr uintptr_t kCreateCallbackPush = 0x004AF758;  // push FUN_004af5c0
constexpr uintptr_t kCreateCallback = 0x004AF5C0;
constexpr uintptr_t kDropdownAdd = 0x0041B760;
constexpr uintptr_t kDropdownSelectText = 0x0041A890;
constexpr uintptr_t kActivatedId = 0x006D051C;  // id of the button just activated
constexpr int16_t kOkId = 12;
constexpr int16_t kPanelId = 1;
constexpr int kPanelW = 363, kPanelH = 332;          // pListMap.pcx
constexpr int kRowSource = 283, kRowY = 328, kRowH = 22;  // Teams row band; first new row's controls; row pitch
constexpr int kBorderY = 325;  // where the image is split: the bottom border piece is rows 325..331
constexpr int16_t kGameTypeLabelId = 0x26, kGameTypeDropdownId = 0x11;
// Negative ids: FUN_00418100 gives every control with id > 0 the screen's handler table[id - 1] (0x51AA08, 38
// entries, no bounds check), so a new positive id picks up whatever lies past the table (seen in a user test:
// garbage event procs, and the dropdown never initialized). Ids <= 0 keep their own procs, like the screen's
// unnamed labels (-1); other negative ids stay focusable (FUN_00418030 only excludes -1).
constexpr int16_t kLabelId = -1;
constexpr size_t kRecordSize = 0x56;
uint32_t g_modalLoop = kModalLoop, g_dropdownAdd = kDropdownAdd;

struct Item {
  const char* text;
  int value;
};
constexpr Item kLimitItems[] = {{"12 units", 12}, {"24 units", 24}};
constexpr Item kZoomItems[] = {{"Enabled", 1}, {"Disabled", 0}};

// One added row: a label and a dropdown whose item data is the rule's value.
struct Row {
  char* labelText;
  const char* regValue;  // also its name in the log
  const Item* items;
  size_t count;
  int (*get)();          // the current choice
  void (*set)(int);
  int16_t dropdownId;
  uint8_t* label;  // the cloned records
  uint8_t* dropdown;
  bool initTried;
  bool filled;  // once per screen: after the dialog is destroyed the arrays are freed but not cleared (FUN_0041b8f0)
};

alignas(4) uint8_t g_records[3][2][(kRecordSize + 3) & ~size_t{3}];  // each record 4-aligned
char g_limitLabel[] = "Selection Limit";
char g_zoomLabel[] = "Zooming:";
char g_rallyLabel[] = "Rally Mining:";
Row g_selectionRow = {g_limitLabel, "Selection Limit", kLimitItems, std::size(kLimitItems), SelectionPreferredLimit,
                      SelectionSetPreferredLimit, -2, g_records[0][0], g_records[0][1]};
Row g_zoomRow = {g_zoomLabel, "Zooming", kZoomItems, std::size(kZoomItems), [] { return g_zoomChoice ? 1 : 0; },
                 [](int value) { g_zoomChoice = value != 0; }, -3, g_records[1][0], g_records[1][1]};
Row g_rallyRow = {g_rallyLabel, "Rally Mining", kZoomItems, std::size(kZoomItems),
                  [] { return RallyMiningPreferred() ? 1 : 0; }, [](int value) { RallyMiningSetPreferred(value != 0); },
                  -4, g_records[2][0], g_records[2][1]};
Row* const kAllRows[] = {&g_selectionRow, &g_zoomRow, &g_rallyRow};

Row* g_rows[std::size(kAllRows)];  // the rows on the screen now
int g_rowCount;
bool g_rowLinked;
uint8_t* g_gameTypeDropdown;  // the screen's own dropdown: initialized means ours should be too

// Kept like Game Speed (FUN_004ae320 / FUN_004ae3c0): Storm SRegLoadString / SRegSaveString (ordinals 422 / 425)
// under the game's "Starcraft" key, holding the chosen item's text.
constexpr char kRegKey[] = "Starcraft";
using SRegLoadStringFn = BOOL(WINAPI*)(const char*, const char*, uint32_t, char*, uint32_t);
using SRegSaveStringFn = BOOL(WINAPI*)(const char*, const char*, uint32_t, const char*);

// The saved choice, applied as the starting value; false if none (then the configured default stands).
bool LoadSaved(Row& row) {
  auto load = reinterpret_cast<SRegLoadStringFn>(GetProcAddress(GetModuleHandleA("storm.dll"), MAKEINTRESOURCEA(422)));
  char saved[128] = {};
  if (!load || !load(kRegKey, row.regValue, 0, saved, sizeof(saved))) return false;
  for (size_t i = 0; i < row.count; ++i) {
    if (_stricmp(saved, row.items[i].text) == 0) {
      row.set(row.items[i].value);
      return true;
    }
  }
  return false;
}

void Save(const Row& row, const char* text) {
  auto save = reinterpret_cast<SRegSaveStringFn>(GetProcAddress(GetModuleHandleA("storm.dll"), MAKEINTRESOURCEA(425)));
  if (!save || !save(kRegKey, row.regValue, 0, text)) log::Error("rules: could not save the %s choice", row.regValue);
}

template <typename T>
T& Field(uint8_t* record, int offset) {
  return *reinterpret_cast<T*>(record + offset);
}

// Copies `from` into `record` at the given rect (left, top, size) with a new id and text.
void CloneControl(uint8_t* record, uint8_t* from, int16_t x, int16_t y, int16_t w, int16_t h, int16_t id, char* text) {
  std::memcpy(record, from, kRecordSize);
  Field<uint8_t*>(record, 0) = nullptr;
  for (int off : {0x04, 0x46}) {
    if (off == 0x46 && Field<int16_t>(from, 0x22) == 13) break;  // dropdowns keep list state there
    Field<int16_t>(record, off) = x;
    Field<int16_t>(record, off + 2) = y;
    Field<int16_t>(record, off + 4) = x + w - 1;
    Field<int16_t>(record, off + 6) = y + h - 1;
  }
  Field<int16_t>(record, 0x0C) = w;
  Field<int16_t>(record, 0x0E) = h;
  Field<int16_t>(record, 0x20) = id;
  Field<char*>(record, 0x14) = text;
}

// Storm SMemAlloc / SMemFree (ordinals 401 / 403), stdcall (size or pointer, source file, line, flags).
using SMemAllocFn = void*(WINAPI*)(uint32_t, const char*, int, uint32_t);
using SMemFreeFn = BOOL(WINAPI*)(void*, const char*, int, uint32_t);

// Makes the panel image `rows` rows taller (see above). False if the panel is not the expected image.
bool ExpandPanel(uint8_t* panel, int rows) {
  int16_t w = Field<int16_t>(panel, 0x0C), h = Field<int16_t>(panel, 0x0E);
  uint8_t* pixels = Field<uint8_t*>(panel, 0x10);
  if (w != kPanelW || h != kPanelH || !pixels) {
    log::Error("rules: create screen panel is %dx%d (expected %dx%d); not extended", w, h, kPanelW, kPanelH);
    return false;
  }
  HMODULE storm = GetModuleHandleA("storm.dll");
  auto alloc = reinterpret_cast<SMemAllocFn>(GetProcAddress(storm, MAKEINTRESOURCEA(401)));
  auto release = reinterpret_cast<SMemFreeFn>(GetProcAddress(storm, MAKEINTRESOURCEA(403)));
  int added = rows * kRowH;
  auto* taller = alloc ? static_cast<uint8_t*>(alloc(w * (h + added), "scpatch rules.cpp", __LINE__, 0)) : nullptr;
  if (!taller || !release) return false;
  std::memcpy(taller, pixels, w * kBorderY);
  for (int i = 0; i < rows; ++i)
    std::memcpy(taller + w * (kBorderY + i * kRowH), pixels + w * kRowSource, w * kRowH);
  std::memcpy(taller + w * (kBorderY + added), pixels + w * kBorderY, w * (h - kBorderY));
  Field<uint8_t*>(panel, 0x10) = taller;
  Field<int16_t>(panel, 0x0E) = static_cast<int16_t>(h + added);
  Field<int16_t>(panel, 0x0A) = static_cast<int16_t>(Field<int16_t>(panel, 0x0A) + added);  // rect bottom
  release(pixels, "scpatch rules.cpp", __LINE__, 0);
  return true;
}

void __cdecl AddRuleRows(uint8_t* root) {
  g_rowLinked = false;
  g_rowCount = 0;
  for (Row* row : kAllRows) row->initTried = row->filled = false;
  if (!root || !*reinterpret_cast<uint8_t*>(0x0057F0B4)) return;
  uint8_t *dropdown = nullptr, *label = nullptr, *panel = nullptr, *last = nullptr;
  for (uint8_t* c = Field<uint8_t*>(root, 0x42); c; c = Field<uint8_t*>(c, 0)) {
    int16_t id = Field<int16_t>(c, 0x20), type = Field<int16_t>(c, 0x22);
    if (id == kPanelId && type == 5) panel = c;
    if (id == kGameTypeDropdownId && type == 13) dropdown = c;
    if (id == kGameTypeLabelId && type == 9) label = c;
    for (Row* row : kAllRows)
      if (c == row->label || c == row->dropdown) return;  // already there
    last = c;
  }
  if (!dropdown || !label || !panel || !last || Field<uint32_t>(dropdown, 0x3A)) {
    log::Error("rules: create screen layout not as expected; no game rule options");
    return;
  }
  if (SelectionAvailable()) g_rows[g_rowCount++] = &g_selectionRow;
  g_rows[g_rowCount++] = &g_zoomRow;
  if (RallyMiningAvailable()) g_rows[g_rowCount++] = &g_rallyRow;
  if (!ExpandPanel(panel, g_rowCount)) return;
  for (int i = 0; i < g_rowCount; ++i) {
    Row& row = *g_rows[i];
    int16_t y = static_cast<int16_t>(kRowY + i * kRowH);
    // Same offsets from the new row as the Teams label (58,284) and dropdown (180,283) from theirs.
    CloneControl(row.label, label, 58, y + 1, 112, 20, kLabelId, row.labelText);
    CloneControl(row.dropdown, dropdown, 180, y, 172, 20, row.dropdownId, nullptr);
    Field<uint8_t*>(last, 0) = row.label;
    Field<uint8_t*>(row.label, 0) = row.dropdown;
    last = row.dropdown;
  }
  g_gameTypeDropdown = dropdown;
  g_rowLinked = true;
}

__declspec(naked) uint8_t __cdecl DropdownAdd(uint8_t* /*dropdown*/, const char* /*text*/) {
  __asm {
    push ebp
    mov ebp, esp
    push ebx
    push esi
    push edi
    mov ecx, [ebp + 8]
    mov eax, [ebp + 12]
    push 0x10
    call dword ptr [g_dropdownAdd]
    movzx eax, al
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret
  }
}

// The dialog's init walk does not reach controls linked after the loaded ones (seen in a user test: the screen's
// dropdowns got flags 0x20020000 and their arrays, ours did not). Once the screen's Game Type dropdown has been
// initialized (FUN_0041a110 sets 0x20000), ours gets the same event: type 0xE, sub-event 10, which allocates the
// arrays (FUN_0041b140) and sets the item height (FUN_0041a110).
void InitDropdown(Row& row) {
  uint8_t* d = row.dropdown;
  if (row.initTried || Field<uint32_t>(d, 0x3A) || !g_gameTypeDropdown ||
      !(Field<uint32_t>(g_gameTypeDropdown, 0x18) & 0x20000))
    return;
  row.initTried = true;  // fallback only: with a negative id the dialog's init walk normally does this
  alignas(4) uint8_t event[0x14] = {};
  Field<uint32_t>(event, 0) = 10;
  Field<uint16_t>(event, 0x0C) = 0x0E;
  uintptr_t proc = Field<uintptr_t>(d, 0x2A);
  uint32_t result = reinterpret_cast<uint32_t(__fastcall*)(uint8_t*, uint8_t*)>(proc)(d, event);
  log::Info("rules: %s dropdown initialized by hand (proc %08X, result %u, flags %08X)", row.regValue,
            static_cast<unsigned>(proc), result, Field<uint32_t>(d, 0x18));
}

// False until the dropdown has been initialized (its arrays allocated); then fills it once.
bool FillDropdown(Row& row) {
  uint8_t* d = row.dropdown;
  InitDropdown(row);
  if (row.filled) return true;
  if (!Field<uint32_t>(d, 0x3A) || !Field<uint32_t>(d, 0x42) || Field<uint8_t>(d, 0x47) < row.count) return false;
  row.filled = true;
  LoadSaved(row);
  const char* current = row.items[0].text;
  for (size_t i = 0; i < row.count; ++i) {
    const Item& item = row.items[i];
    uint8_t index = DropdownAdd(d, item.text);
    if (index == 0xFF) return true;
    Field<uint32_t*>(d, 0x42)[index] = static_cast<uint32_t>(item.value);
    if (item.value == row.get()) current = item.text;
  }
  uint8_t selected = reinterpret_cast<uint8_t(__stdcall*)(uint8_t*, const char*)>(kDropdownSelectText)(d, current);
  log::Info("rules: %s dropdown filled (%u items, selected %d)", row.regValue, Field<uint8_t>(d, 0x46), selected);
  return true;
}

void ReadDropdown(Row& row) {
  uint8_t* d = row.dropdown;
  if (!Field<uint8_t>(d, 0x46)) log::Error("rules: the %s dropdown was never filled", row.regValue);
  uint8_t index = Field<uint8_t>(d, 0x48);
  if (index == 0xFF || index >= Field<uint8_t>(d, 0x46) || !Field<uint32_t>(d, 0x42)) return;
  int value = static_cast<int>(Field<uint32_t*>(d, 0x42)[index]);
  row.set(value);
  for (size_t i = 0; i < row.count; ++i)
    if (row.items[i].value == value) Save(row, row.items[i].text);
}

uint32_t __fastcall CreateCallback(uint8_t* dialog, uint8_t* event) {
  static bool seen;
  if (!seen && g_rowLinked) {
    seen = true;
    log::Info("rules: create screen callback active (first event type %u, sub-event %u)", Field<uint16_t>(event, 0x0C),
              Field<uint32_t>(event, 0));
  }
  bool command = g_rowLinked && Field<uint16_t>(event, 0x0C) == 0x0E;
  if (command && Field<uint32_t>(event, 0) == 2 && *reinterpret_cast<int32_t*>(kActivatedId) == kOkId)
    for (int i = 0; i < g_rowCount; ++i) ReadDropdown(*g_rows[i]);
  // Destroy (sub-event 1): the controls free their data; nothing here may touch them afterwards.
  if (command && Field<uint32_t>(event, 0) == 1) g_rowLinked = false;
  uint32_t result = reinterpret_cast<uint32_t(__fastcall*)(uint8_t*, uint8_t*)>(kCreateCallback)(dialog, event);
  if (command && g_rowLinked)  // from the init event on, once the controls are set up
    for (int i = 0; i < g_rowCount; ++i) FillDropdown(*g_rows[i]);
  return result;
}
// Replaces the modal-loop call: add the rows, then run the loop with the original registers and stack.
__declspec(naked) void CreateModalStub() {
  __asm {
    pushad
    push eax
    call AddRuleRows
    add esp, 4
    popad
    jmp dword ptr [g_modalLoop]
  }
}
// ---- Replay trailer -----------------------------------------------------------------------------------------

#pragma pack(push, 1)
struct Trailer {
  uint8_t magic[4] = {'S', 'C', 'P', 'R'};
  uint8_t version = 1;
  uint8_t size = sizeof(Rules);
};
#pragma pack(pop)

using FcloseFn = int(__cdecl*)(void*);
using FileIoFn = size_t(__cdecl*)(void*, size_t, size_t, void*);

Rules g_replayRules;  // from the replay loaded last

int __cdecl ReplayWriteClose(void* file) {
  Rules rules;
  rules.selectionLimit = static_cast<uint8_t>(SelectionActiveLimit());
  rules.zoom = !*reinterpret_cast<uint8_t*>(0x0057F0B4) || RulesZoomAllowed() ? 1 : 0;  // single-player: allowed
  rules.rallyMining = RallyMiningActive() ? 1 : 0;
  Trailer t;
  auto write = reinterpret_cast<FileIoFn>(kFwrite);
  if (write(&t, sizeof(t), 1, file) != 1 || write(&rules, sizeof(rules), 1, file) != 1)
    log::Error("rules: could not append the rules to the replay");
  return reinterpret_cast<FcloseFn>(kFclose)(file);
}

int __cdecl ReplayReadClose(void* file) {
  auto read = reinterpret_cast<FileIoFn>(kFread);
  Trailer t, expected;
  Rules rules;
  if (read(&t, sizeof(t), 1, file) == 1 && std::memcmp(t.magic, expected.magic, 4) == 0 && t.version >= 1) {
    uint8_t bytes[255] = {};
    if (read(bytes, 1, t.size, file) == t.size) std::memcpy(&rules, bytes, std::min<size_t>(t.size, sizeof(rules)));
  }
  g_replayRules = rules;
  return reinterpret_cast<FcloseFn>(kFclose)(file);
}

std::vector<uint8_t> Dword(uint32_t v) { return {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; }

std::vector<uint8_t> Rel(uint8_t opcode, uintptr_t site, uintptr_t target) {
  std::vector<uint8_t> code = Dword(static_cast<uint32_t>(target - (site + 5)));
  code.insert(code.begin(), opcode);
  return code;
}

std::vector<uint8_t> Push(uintptr_t value) {
  std::vector<uint8_t> code = Dword(static_cast<uint32_t>(value));
  code.insert(code.begin(), 0x68);
  return code;
}

uintptr_t Addr(const void* p) { return reinterpret_cast<uintptr_t>(p); }

patch::PatchSet g_hooks("rules");

}  // namespace

bool RulesHosting() {
  std::lock_guard<std::mutex> hold(g_lock);
  if (g_role != Role::None) return g_role == Role::Host;
  return *reinterpret_cast<uint32_t*>(kOwnPlayer) == 0;
}

int RulesSelectionLimit() {
  std::lock_guard<std::mutex> hold(g_lock);
  // Host: only rules frozen at Start count. Joiner: the last rules the host sent (vanilla if none arrived).
  if (g_role == Role::Host && !g_frozen) return kVanillaLimit;
  return g_role == Role::None ? kVanillaLimit : g_current.selectionLimit;
}

bool RulesZoomAllowed() {
  std::lock_guard<std::mutex> hold(g_lock);
  if (g_role == Role::Host && !g_frozen) return false;
  return g_role != Role::None && g_current.zoom != 0;
}

bool RulesRallyMining() {
  std::lock_guard<std::mutex> hold(g_lock);
  if (g_role == Role::Host && !g_frozen) return false;
  return g_role != Role::None && g_current.rallyMining != 0;
}

int RulesReplaySelectionLimit() { return g_replayRules.selectionLimit; }
bool RulesReplayRallyMining() { return g_replayRules.rallyMining != 0; }

void InstallRules() {
  HMODULE storm = GetModuleHandleA("storm.dll");
  g_send = storm ? reinterpret_cast<SendFn>(GetProcAddress(storm, MAKEINTRESOURCEA(kOrdSendMessage))) : nullptr;
  if (!g_send) {
    log::Error("rules: storm.dll SNetSendMessage not found; lobby rules disabled");
    return;
  }

  g_hooks.Add(kCreateCall, Rel(0xE8, kCreateCall, kCreateThunk), Rel(0xE8, kCreateCall, Addr(&CreateGame)),
              "create game");
  g_hooks.Add(kJoinCall, Rel(0xE8, kJoinCall, kJoinThunk), Rel(0xE8, kJoinCall, Addr(&JoinGame)), "join game");
  g_hooks.Add(kLeftHandlerPush, Push(kLeftHandler), Push(Addr(&PlayerLeft)), "player left handler");
  g_hooks.Add(kJoinedHandlerPush, Push(kJoinedHandler), Push(Addr(&PlayerJoined)), "player joined handler");
  g_hooks.Add(kStartCall, Rel(0xE8, kStartCall, kStartRequest), Rel(0xE9, kStartCall, Addr(&StartStub)),
              "start button");
  g_hooks.Add(kCreateModalCall, Rel(0xE8, kCreateModalCall, kModalLoop),
              Rel(0xE8, kCreateModalCall, Addr(&CreateModalStub)), "create screen: rule rows");
  g_hooks.Add(kCreateCallbackPush, Push(kCreateCallback), Push(Addr(&CreateCallback)), "create screen: callback");
  g_hooks.Add(kReplayWriteClose, Rel(0xE8, kReplayWriteClose, kFclose),
              Rel(0xE8, kReplayWriteClose, Addr(&ReplayWriteClose)), "replay write: rules trailer");
  g_hooks.Add(kReplayReadClose, Rel(0xE8, kReplayReadClose, kFclose),
              Rel(0xE8, kReplayReadClose, Addr(&ReplayReadClose)), "replay read: rules trailer");
  if (!g_hooks.Verify()) {
    log::Error("rules: not installed");
    return;
  }
  if (!patch::HookImport(GetModuleHandleA(nullptr), "storm.dll", nullptr, reinterpret_cast<void*>(&ReceiveMessage),
                         reinterpret_cast<void**>(&g_receive), kOrdReceiveMessage)) {
    log::Error("rules: SNetReceiveMessage import not found; lobby rules disabled");
    return;
  }
  g_hooks.Enable(true);
  log::Info("rules: lobby rules installed (hosting asks for selection limit %d)", SelectionPreferredLimit());
}

}  // namespace scpatch::features
