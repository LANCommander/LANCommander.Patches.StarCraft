// Match statistics for LANCommander, through the embedded Interposer's InterposerMatch* API (re/notes/matches.md).
//
// Only the host of a multiplayer game reports (RulesHosting), so every game reaches LANCommander once. Where the
// report goes is the Interposer's configuration, not ours: MatchStatistics in .interposer\Config.yml (ServerUrl,
// Token, GameId) sends it straight to the server; without them, LANCommander's launcher delivers it when it
// started the game; otherwise it is only written to the Interposer's log. We declare ourselves ReporterKind Host.
//
//   * Start: a poll thread (kPollMs) sees the in-game loop begin (0x6D11EC) in a multiplayer game (0x57F0B4) that
//     is not a replay (0x6D0F14), and starts the match from the player table: humans and computers, race, force.
//   * During the match the same thread watches each player's end state (0x58D700, written by FUN_0048a200 /
//     FUN_0048a180 / FUN_0048a1c0) and reports player_left / player_defeated / player_victory / player_draw.
//   * End: the game's teardown FUN_004ee8c0 runs after every game (both calls in RunGameState are wrapped). At its
//     start the end states and the score arrays are still intact (the Battle.net result report FUN_004c4790 reads
//     them there). Results, the score screen's totals per player and the end are reported then. The game's own
//     result report runs only on Battle.net (provider 'BNET'), so it cannot be used for LAN games.
//   * After the teardown, which writes LastReplay.rep (FUN_004dfab0), a copy of the replay is attached.

#include <windows.h>

#include <LANCommander.Interposer.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

#include "core/config.h"
#include "core/log.h"
#include "core/patch.h"
#include "core/paths.h"
#include "features/features.h"

namespace scpatch::features {
namespace {

// Player table: 12 records of 0x24 bytes. +8 type, +9 race, +0xA force, +0xB name (25 chars, verified by the
// lobby's name copies, Storm ordinal 501/113 with 0x19). Types (as FUN_004b31c0 selects the score screen's
// players): 1 computer, 2 human, 10 human that left, 11 computer that left.
constexpr uintptr_t kPlayers = 0x0057EEE0;
constexpr size_t kPlayerSize = 0x24;
constexpr int kSlots = 8;  // the game ignores players >= 8 in multiplayer
// End state per player slot (u8): 0 playing, 1 left (FUN_0048a180), 2 defeated (FUN_0048a1c0), 3 victorious,
// 5 draw. Mapped to Storm results in FUN_004c4220: 3 win, 2 loss, 5 draw, 1 disconnect.
constexpr uintptr_t kEndState = 0x0058D700;
constexpr uintptr_t kLocalSlot = 0x00512684;
constexpr uintptr_t kMultiplayer = 0x0057F0B4;
constexpr uintptr_t kReplayActive = 0x006D0F14;
constexpr uintptr_t kInGameLoop = 0x006D11EC;
constexpr uintptr_t kFrameCount = 0x0057F23C;
constexpr uintptr_t kMapTitle = 0x0057FE40;  // "<map>%s</map>" in the score report FUN_004b3220
constexpr uintptr_t kMapFile = 0x0057FD3C;
constexpr int kMsPerFrame = 42;  // a game frame at Fastest: game time as the game counts it

// Score arrays: u32[12] by player slot. Read the way the score screen's report (FUN_004b2a90..FUN_004b2b70)
// reads them; the names are the XML attributes it writes.
constexpr uintptr_t kMineralsNow = 0x0057F0F0;
constexpr uintptr_t kGasNow = 0x0057F120;
constexpr uintptr_t kGasMined = 0x0057F150;
constexpr uintptr_t kMineralsMined = 0x0057F180;
constexpr uintptr_t kUnitsProduced = 0x00581E44;
constexpr uintptr_t kUnitsLost = 0x00581E74;
constexpr uintptr_t kUnitsKilled = 0x00581EA4;
constexpr uintptr_t kUnitScoreA = 0x00581ED4;  // units score = A + B
constexpr uintptr_t kUnitScoreB = 0x00581F04;
constexpr uintptr_t kStructuresConstructed = 0x00581F94;
constexpr uintptr_t kStructuresLost = 0x00581FC4;
constexpr uintptr_t kStructuresRazed = 0x00581FF4;
constexpr uintptr_t kStructureScoreA = 0x00582024;  // structures score = A + B
constexpr uintptr_t kStructureScoreB = 0x00582054;

constexpr uintptr_t kTeardown = 0x004EE8C0;
constexpr uintptr_t kTeardownCalls[] = {0x004E07FC, 0x004E080F};  // RunGameState

constexpr DWORD kPollMs = 250;
constexpr DWORD kReplayFreshMs = 60 * 1000;  // LastReplay.rep written by this teardown, not an old one
constexpr int kReplayKeepDays = 14;

template <typename T>
T Read(uintptr_t address) {
  return *reinterpret_cast<volatile T*>(address);
}
const uint8_t* PlayerRecord(int slot) { return reinterpret_cast<const uint8_t*>(kPlayers + slot * kPlayerSize); }
uint8_t EndState(int slot) { return Read<uint8_t>(kEndState + slot); }
uint32_t Score(uintptr_t array, int slot) { return Read<uint32_t>(array + slot * 4); }
uint32_t Frames() { return Read<uint32_t>(kFrameCount); }
long long GameMs(uint32_t frames) { return static_cast<long long>(frames) * kMsPerFrame; }

// Game text (the ANSI code page) as UTF-8, without the colour/formatting bytes below 0x20.
std::string Utf8(const char* text, size_t max) {
  std::string clean;
  for (size_t i = 0; i < max && text[i]; ++i)
    if (static_cast<uint8_t>(text[i]) >= 0x20) clean += text[i];
  if (clean.empty()) return clean;
  int wide = MultiByteToWideChar(CP_ACP, 0, clean.data(), static_cast<int>(clean.size()), nullptr, 0);
  std::wstring w(wide, L'\0');
  MultiByteToWideChar(CP_ACP, 0, clean.data(), static_cast<int>(clean.size()), w.data(), wide);
  int bytes = WideCharToMultiByte(CP_UTF8, 0, w.data(), wide, nullptr, 0, nullptr, nullptr);
  std::string out(bytes, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), wide, out.data(), bytes, nullptr, nullptr);
  return out;
}

std::string JsonString(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      default:
        if (static_cast<uint8_t>(c) < 0x20) {
          char esc[8];
          snprintf(esc, sizeof(esc), "\\u%04x", static_cast<uint8_t>(c));
          out += esc;
        } else {
          out += c;
        }
    }
  }
  return out + "\"";
}

const char* RaceName(uint8_t race) {
  switch (race) {
    case 0: return "zerg";
    case 1: return "terran";
    case 2: return "protoss";
    default: return "unknown";
  }
}

struct Player {
  bool present = false;
  bool human = false;
  uint8_t force = 0;
  uint8_t state = 0;      // last end state reported
  bool inferredWin = false;
  uint32_t eliminatedAt = 0;  // frame of player_left / player_defeated
  std::string key, name, race, team;
};

enum class Phase { Idle, Open, Done };  // Done: this game is handled (or not ours); wait for the next one

std::mutex g_lock;
Phase g_phase = Phase::Idle;
DWORD g_match;
bool g_teams;
Player g_players[kSlots];
bool g_attachReplay = true;
DWORD g_lastMatch;  // the match whose replay the teardown attaches

bool Reportable(std::string* why) {
  if (!Read<uint8_t>(kMultiplayer)) return *why = "single-player", false;
  if (Read<uint32_t>(kReplayActive)) return *why = "replay", false;
  if (!RulesHosting()) return *why = "not the host (the host reports)", false;
  return true;
}

std::string SettingsJson() {
  char json[160];
  snprintf(json, sizeof(json), "{\"selection_limit\":%d,\"zooming\":%s,\"rally_mining\":%s}", RulesSelectionLimit(),
           RulesZoomAllowed() ? "true" : "false", RulesRallyMining() ? "true" : "false");
  return json;
}

// Under the lock.
void StartMatch() {
  for (int slot = 0; slot < kSlots; ++slot) {
    Player& p = g_players[slot];
    p = Player{};
    const uint8_t* rec = PlayerRecord(slot);
    uint8_t type = rec[8];
    if (type != 1 && type != 2) continue;  // also skips players that left before the start
    p.present = true;
    p.human = type == 2;
    p.force = rec[0xA];
    p.race = RaceName(rec[9]);
    p.name = Utf8(reinterpret_cast<const char*>(rec + 0xB), 25);
    if (p.human && !p.name.empty()) {
      p.key = "name:" + p.name;
    } else {
      p.key = "bot:" + std::to_string(slot);
      if (p.name.empty()) p.name = "Computer " + std::to_string(slot + 1);
    }
    p.state = EndState(slot);
  }
  // Melee maps put every player in one force; a game with more than one force is a team game.
  g_teams = false;
  int firstForce = -1;
  for (const Player& p : g_players) {
    if (!p.present) continue;
    if (firstForce < 0) firstForce = p.force;
    else if (p.force != firstForce) g_teams = true;
  }
  if (g_teams)
    for (Player& p : g_players)
      if (p.present) p.team = "Force " + std::to_string(p.force + 1);

  std::string map = Utf8(reinterpret_cast<const char*>(kMapTitle), 32);
  std::string file = Utf8(reinterpret_cast<const char*>(kMapFile), MAX_PATH);
  if (size_t cut = file.find_last_of("\\/"); cut != std::string::npos) file.erase(0, cut + 1);
  std::string meta = "{\"patch\":\"scpatch\",\"map_file\":" + JsonString(file) + ",\"players\":[";
  bool first = true;
  for (int slot = 0; slot < kSlots; ++slot) {
    const Player& p = g_players[slot];
    if (!p.present) continue;
    meta += first ? "" : ",";
    first = false;
    meta += "{\"key\":" + JsonString(p.key) + ",\"slot\":" + std::to_string(slot) +
            ",\"race\":" + JsonString(p.race) + ",\"force\":" + std::to_string(p.force) +
            ",\"human\":" + (p.human ? "true" : "false") + "}";
  }
  meta += "]}";
  std::string settings = SettingsJson();

  InterposerMatchInfo info = {sizeof(info)};
  info.map = map.c_str();
  info.mode = g_teams ? "team" : "free_for_all";
  info.gameVersion = "1.16.1";
  info.settingsJson = settings.c_str();
  info.metadataJson = meta.c_str();
  g_match = InterposerMatchStart(&info);
  if (!g_match) {
    log::Error("matches: InterposerMatchStart refused the match (map \"%s\")", map.c_str());
    g_phase = Phase::Done;
    return;
  }
  int count = 0;
  for (const Player& p : g_players) {
    if (!p.present) continue;
    ++count;
    InterposerMatchPlayerJoined(g_match, p.key.c_str(), p.name.c_str(), p.team.empty() ? nullptr : p.team.c_str(), 0);
  }
  InterposerMatchStatus status = {sizeof(status)};
  InterposerMatchGetStatus(g_match, &status);
  static const char* const kSinks[] = {"log only (no MatchStatistics server and not started by the launcher)",
                                       "LANCommander server", "LANCommander launcher"};
  log::Info("matches: match %lu started on \"%s\", %d players, %s; delivered by %s", g_match, map.c_str(), count,
            g_teams ? "teams" : "free for all", status.sink >= 0 && status.sink <= 2 ? kSinks[status.sink] : "?");
  g_phase = Phase::Open;
}

void Event(const char* type, int slot, uint32_t frames, bool inferred = false) {
  const Player& p = g_players[slot];
  std::string payload = "{\"slot\":" + std::to_string(slot) + ",\"race\":" + JsonString(p.race) +
                        (inferred ? ",\"inferred\":true}" : "}");
  InterposerMatchEvent(g_match, type, GameMs(frames), frames, p.key.c_str(), nullptr, payload.c_str());
  unsigned seconds = static_cast<unsigned>(GameMs(frames) / 1000);
  log::Info("matches: %s: %s at %u:%02u", type, p.name.c_str(), seconds / 60, seconds % 60);
}

// Reports each player whose end state changed. Under the lock, match open.
void PollStates() {
  uint32_t frames = Frames();
  for (int slot = 0; slot < kSlots; ++slot) {
    Player& p = g_players[slot];
    uint8_t state = EndState(slot);
    if (!p.present || state == p.state) continue;
    p.state = state;
    switch (state) {
      case 1:
        p.eliminatedAt = frames;
        Event("player_left", slot, frames);
        InterposerMatchPlayerLeft(g_match, p.key.c_str(), GameMs(frames));
        break;
      case 2:
        p.eliminatedAt = frames;
        Event("player_defeated", slot, frames);
        break;
      case 3: Event("player_victory", slot, frames); break;
      case 5: Event("player_draw", slot, frames); break;
      default: break;
    }
  }
}

void SetStat(const Player& p, const char* key, double value) {
  InterposerMatchSetStatistic(g_match, p.key.c_str(), nullptr, key, value);
}

// Under the lock, match open. At the start of the teardown: end states and scores are final.
void EndMatch() {
  PollStates();
  uint32_t frames = Frames();
  int local = static_cast<int>(Read<uint32_t>(kLocalSlot));

  // The host quitting mid-game leaves it "playing"; it left. As the game itself decides for its result report
  // (FUN_004c4220): if exactly one other human is still playing and no computer is, that player has won.
  if (local >= 0 && local < kSlots && g_players[local].present && g_players[local].state == 0) {
    Player& host = g_players[local];
    host.state = 1;
    host.eliminatedAt = frames;
    Event("player_left", local, frames);
    InterposerMatchPlayerLeft(g_match, host.key.c_str(), GameMs(frames));
  }
  int playingHumans = 0, playingComputers = 0, last = -1;
  for (int slot = 0; slot < kSlots; ++slot) {
    const Player& p = g_players[slot];
    if (!p.present || p.state != 0) continue;
    (p.human ? playingHumans : playingComputers)++;
    last = slot;
  }
  bool anyWinner = false;
  for (const Player& p : g_players) anyWinner |= p.present && (p.state == 3 || p.state == 5);
  if (!anyWinner && playingHumans == 1 && playingComputers == 0) {
    g_players[last].inferredWin = true;
    Event("player_victory", last, frames, true);
    anyWinner = true;
  }

  int players = 0;
  for (int slot = 0; slot < kSlots; ++slot) {
    const Player& p = g_players[slot];
    if (!p.present) continue;
    ++players;
    uint32_t minerals = Score(kMineralsMined, slot), gas = Score(kGasMined, slot);
    uint32_t mined = minerals + gas;
    uint32_t held = Score(kMineralsNow, slot) + Score(kGasNow, slot);
    uint32_t unitScore = Score(kUnitScoreA, slot) + Score(kUnitScoreB, slot);
    uint32_t structureScore = Score(kStructureScoreA, slot) + Score(kStructureScoreB, slot);
    uint32_t total = unitScore + structureScore + mined;
    SetStat(p, "minerals_mined", minerals);
    SetStat(p, "gas_mined", gas);
    SetStat(p, "resources_spent", mined > held ? mined - held : 0);
    SetStat(p, "units_produced", Score(kUnitsProduced, slot));
    SetStat(p, "units_killed", Score(kUnitsKilled, slot));
    SetStat(p, "units_lost", Score(kUnitsLost, slot));
    SetStat(p, "structures_constructed", Score(kStructuresConstructed, slot));
    SetStat(p, "structures_razed", Score(kStructuresRazed, slot));
    SetStat(p, "structures_lost", Score(kStructuresLost, slot));
    SetStat(p, "unit_score", unitScore);
    SetStat(p, "structure_score", structureScore);
    SetStat(p, "resource_score", mined);
    SetStat(p, "total_score", total);

    // Winners first; the rest by when they were eliminated (in a free-for-all the last one out places second).
    // A team game has winners and losers only. Players still playing when the host's game ended get no place.
    bool won = p.state == 3 || p.state == 5 || p.inferredWin;
    if (!won && p.state != 1 && p.state != 2) continue;
    InterposerMatchResult r = {sizeof(r)};
    r.playerKey = p.key.c_str();
    r.team = p.team.empty() ? nullptr : p.team.c_str();
    r.timeMs = -1;
    r.hasScore = 1;
    r.score = total;
    if (won) {
      r.rank = 1;
      r.status = INTERPOSER_MATCH_RESULT_FINISHED;
    } else {
      r.rank = 2;
      if (!g_teams)
        for (const Player& o : g_players)
          if (o.present && (o.state == 1 || o.state == 2) && !o.inferredWin && o.eliminatedAt > p.eliminatedAt) ++r.rank;
      r.status = p.state == 1 ? INTERPOSER_MATCH_RESULT_FORFEITED : INTERPOSER_MATCH_RESULT_DEFEATED;
    }
    std::string payload = "{\"race\":" + JsonString(p.race) + ",\"slot\":" + std::to_string(slot) +
                          (p.state == 5 ? ",\"draw\":true}" : "}");
    r.payloadJson = payload.c_str();
    InterposerMatchSetResult(g_match, &r);
  }

  // Without a winner the outcome is unknown (the host left a game others play on): the results do not count.
  const char* reason = anyWinner ? "completed" : "host_left";
  InterposerMatchEnd(g_match, reason, anyWinner ? FALSE : TRUE);
  unsigned seconds = static_cast<unsigned>(GameMs(frames) / 1000);
  log::Info("matches: match %lu ended (%s) after %u:%02u, %d players%s", g_match, reason, seconds / 60, seconds % 60,
            players,
            anyWinner ? "" : "; marked abandoned, no winner known");
  g_lastMatch = g_match;
  g_match = 0;
  g_phase = Phase::Done;
}

// The newest LastReplay.rep, if it was written just now: the save-path folder, or the game's own.
std::wstring FreshReplay() {
  std::wstring best;
  ULONGLONG bestTime = 0;
  for (const std::wstring& dir : {paths::UserDataRoot() + L"\\Replays", paths::GameDir() + L"\\maps\\replays"}) {
    std::wstring path = dir + L"\\LastReplay.rep";
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) continue;
    ULONGLONG t = (static_cast<ULONGLONG>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
    if (t > bestTime) bestTime = t, best = path;
  }
  FILETIME nowFt;
  GetSystemTimeAsFileTime(&nowFt);
  ULONGLONG now = (static_cast<ULONGLONG>(nowFt.dwHighDateTime) << 32) | nowFt.dwLowDateTime;
  if (best.empty() || now < bestTime || (now - bestTime) / 10000 > kReplayFreshMs) return {};
  return best;
}

std::wstring ReplayCopyDir() { return paths::UserDataRoot() + L"\\Matches"; }

// Copies kept for upload are deleted after kReplayKeepDays (the Interposer uploads them within the session, or
// the next run if it was cut short).
void PruneReplayCopies() {
  WIN32_FIND_DATAW find;
  HANDLE h = FindFirstFileW((ReplayCopyDir() + L"\\*.rep").c_str(), &find);
  if (h == INVALID_HANDLE_VALUE) return;
  FILETIME nowFt;
  GetSystemTimeAsFileTime(&nowFt);
  ULONGLONG now = (static_cast<ULONGLONG>(nowFt.dwHighDateTime) << 32) | nowFt.dwLowDateTime;
  constexpr ULONGLONG kDay = 24ull * 60 * 60 * 10000000;
  do {
    ULONGLONG t = (static_cast<ULONGLONG>(find.ftLastWriteTime.dwHighDateTime) << 32) | find.ftLastWriteTime.dwLowDateTime;
    if (now > t && now - t > kReplayKeepDays * kDay) DeleteFileW((ReplayCopyDir() + L"\\" + find.cFileName).c_str());
  } while (FindNextFileW(h, &find));
  FindClose(h);
}

// After the teardown: the replay it wrote, copied (the next game overwrites LastReplay.rep) and attached.
void AttachReplay(DWORD match, long long endMs) {
  InterposerMatchStatus status = {sizeof(status)};
  if (!InterposerMatchGetStatus(match, &status) || status.sink == INTERPOSER_MATCH_SINK_NONE) return;
  std::wstring replay = FreshReplay();
  if (replay.empty()) {
    log::Info("matches: no fresh LastReplay.rep to attach to match %lu", match);
    return;
  }
  SYSTEMTIME t;
  GetLocalTime(&t);
  wchar_t name[64];
  swprintf_s(name, L"\\%04u%02u%02u-%02u%02u%02u-%lu.rep", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
             match);
  std::wstring copy = ReplayCopyDir() + name;
  if (!paths::EnsureDirectory(ReplayCopyDir()) || !CopyFileW(replay.c_str(), copy.c_str(), FALSE)) {
    log::Error("matches: could not copy %ls for upload (error %lu)", replay.c_str(), GetLastError());
    return;
  }
  if (InterposerMatchAttachFile(match, copy.c_str(), INTERPOSER_MATCH_ATTACHMENT_REPLAY, 0, endMs))
    log::Info("matches: replay %ls attached to match %lu", copy.c_str(), match);
}

void __cdecl Teardown() {
  DWORD ended = 0;
  long long endMs = 0;
  {
    std::lock_guard<std::mutex> hold(g_lock);
    // A game the poll thread did not see start (it would have to be under kPollMs long).
    std::string why;
    if (g_phase == Phase::Idle && Frames() > 0 && Reportable(&why)) StartMatch();
    if (g_phase == Phase::Open) {
      endMs = GameMs(Frames());
      EndMatch();
      ended = g_lastMatch;
    }
    g_phase = Phase::Done;  // until the in-game loop has stopped (it already has, normally)
  }
  reinterpret_cast<void (*)()>(kTeardown)();
  if (ended && g_attachReplay) AttachReplay(ended, endMs);
}

DWORD WINAPI PollThread(void*) {
  for (;;) {
    Sleep(kPollMs);
    bool inGame = Read<uint8_t>(kInGameLoop) != 0;
    std::lock_guard<std::mutex> hold(g_lock);
    if (!inGame) {
      if (g_phase == Phase::Done) g_phase = Phase::Idle;
      continue;
    }
    if (g_phase == Phase::Idle) {
      std::string why;
      if (Reportable(&why)) {
        StartMatch();
      } else {
        if (Read<uint8_t>(kMultiplayer)) log::Info("matches: not reporting this game: %s", why.c_str());
        g_phase = Phase::Done;
      }
    }
    if (g_phase == Phase::Open) PollStates();
  }
}

}  // namespace

void InstallMatches() {
  if (!config::GetBool(L"Matches", L"Enabled", true)) {
    log::Info("matches: disabled ([Matches] Enabled=0)");
    return;
  }
  if (!(InterposerGetFeatures() & INTERPOSER_FEATURE_FLAG_MATCHES)) {
    log::Info("matches: the Interposer was built without match statistics; not reporting");
    return;
  }
  g_attachReplay = config::GetBool(L"Matches", L"AttachReplay", true);

  // We report as the game's host. Everything else (server, token, game id) stays with Config.yml / the launcher.
  InterposerMatchConfig settings = {sizeof(settings)};
  settings.enabled = -1;
  settings.reporterKind = "Host";
  if (!InterposerMatchConfigure(&settings)) log::Error("matches: could not set ReporterKind Host");

  for (uintptr_t site : kTeardownCalls)
    if (!patch::RedirectCall(site, kTeardown, reinterpret_cast<void*>(&Teardown), "matches: game teardown")) {
      log::Error("matches: not installed");
      return;
    }
  HANDLE thread = CreateThread(nullptr, 0, PollThread, nullptr, 0, nullptr);
  if (!thread) {
    log::Error("matches: could not start the poll thread (error %lu)", GetLastError());
    return;
  }
  CloseHandle(thread);
  if (g_attachReplay) PruneReplayCopies();
  log::Info("matches: installed; multiplayer games this client hosts are reported%s",
            g_attachReplay ? ", with their replay" : "");
}

}  // namespace scpatch::features
