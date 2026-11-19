#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace scpatch::features {

// Installs every feature enabled in the local config. Game rules are installed separately when a game starts.
void InstallAll();

void InstallSavePaths();
void InstallRenderer();
void InstallNetwork();
void InstallFramePacing();
// 24-unit selections and control groups, applied per game: [Selection] Limit in single-player games, the lobby
// rules in multiplayer.
void InstallSelection();
bool SelectionAvailable();       // installed and verified: this client can play a 24-unit game
int SelectionPreferredLimit();   // [Selection] Limit (12 when unavailable): single-player, and the host's choice
int SelectionActiveLimit();      // the limit applied to the current (or last) game
int SelectionUnits(uint32_t* out);  // copies the local selection (24 slots, some null) into `out`; returns the slot count
void SelectionSetPreferredLimit(int limit);  // from the create-game screen (this session only)
// WM_KEYDOWN in the game window: [Selection] PageKey flips the unit panel between units 1-12 and 13-24.
// True if it was the page key (the message still goes to the game).
bool SelectionKey(uintptr_t key);

// Rally mining: a worker trained with its building's rally point on minerals or its own refinery harvests it.
// Applied per game like the selection limit: [Rally] Mining in single-player, the lobby rules in multiplayer.
void InstallRally();
bool RallyMiningAvailable();   // installed: this client can play a game with rally mining
bool RallyMiningPreferred();   // [Rally] Mining (false when unavailable): single-player, and the host's choice
bool RallyMiningActive();      // applies to the game running now (or the replay being watched)
void RallyMiningSetPreferred(bool enabled);  // from the create-game screen
// [Rally] ShowFlag (local display only): a flag on the map at the rally point of each selected building.

// Hotkey layouts (local): Options > Controls in the in-game menu switches the command card between the game's
// hotkeys and a grid (Q W E / A S D / Z X C = the 3x3 card). [Controls] Hotkeys until the menu's choice is saved.
void InstallControls();

// Lobby game rules: the host's rules reach every patched player before the game starts, and apply only if every
// player has this patch (else vanilla rules for everyone). Call after the features whose rules it carries.
void InstallRules();
// This client created the current (or last) multiplayer game. Before any create/join, and if the rules could not
// be installed: Storm player 0, which is the creator.
bool RulesHosting();
int RulesSelectionLimit();  // the agreed selection limit for the multiplayer game starting now
bool RulesZoomAllowed();    // the multiplayer game starting now allows zooming (the host's "Zooming" choice)
bool RulesRallyMining();    // the multiplayer game starting now has rally mining (the host's choice)
// The selection limit stored in the replay loaded last (12 for replays without scpatch's rules trailer).
int RulesReplaySelectionLimit();
bool RulesReplayRallyMining();  // off for replays without the rule

// Match statistics for LANCommander through the embedded Interposer ([Matches]): the host of a multiplayer game
// reports its players, eliminations, results, score-screen totals and the replay. Joiners report nothing, so each
// game is reported once. Call after InstallRules.
void InstallMatches();
void InstallWidescreen();
void DumpWidescreenState(const wchar_t* path);
bool WidescreenActive();
int WidescreenWidth();
int WidescreenScreenHeight();   // in-match screen height, 480 + the HUD size's extra rows (480 when inactive)
int WidescreenMapAreaHeight();  // the part of it above the HUD where the map shows, screen height - 80
// HUD size (local): in percent of the normal size, 100 down to 60. A smaller HUD is a taller in-match screen
// with the HUD moved to its bottom. The in-game menu's choice (Options > Controls) applies at once and is saved.
bool HudSizeAvailable();
int HudSizeActive();
int HudSizeChosen();
int HudSizeNext(int percent);  // the next size offered after `percent` (wraps around)
void HudSizeChoose(int percent);
uint8_t* WidescreenDirtyGrid();  // W/16 cells per row, 30 rows
size_t WidescreenDirtyGridBytes();
// The map view the game is patched for: drawn width and height, the map buffer height around it (h + 80), and
// the shown part (top-left aligned, placed with a sub-8-px offset by zoom).
struct MapViewSize {
  int w, h, hs;
  int pw, ph;
};
MapViewSize WidescreenMapView();
MapViewSize WidescreenLargestMapView();
// Repatches the map code for a shown view `height` px high (400 + 32n, up to the largest view) and updates the
// game state derived from it (scroll limits, parallax). Only between map passes; the caller refills the terrain.
bool WidescreenSetMapView(int height);

// Map view heights for zoom when it is enabled: the starting one and the largest one ([Zoom] Level, MinLevel),
// for a map area `areaH` high on a screen `screenH` high (levels keep the map's on-screen scale whatever the HUD
// size). Read by widescreen to size the map code and its buffers.
bool ZoomViewHeights(int areaH, int screenH, int* start, int* largest);
// WM_MOUSEWHEEL during a match: queues zoom steps. True if the wheel is used for zoom (swallow the message).
bool ZoomWheel(int delta);

// Zoom: the map layers draw into their own buffer, the UI layers into the screen buffer (map area keyed).
// Call after InstallWidescreen.
void InstallZoom();
bool ZoomSplitActive();
// The screen changed size during a match (HUD size): takes the new screen and map area, and sets the map view
// for them at the same map scale and centre. Called by widescreen between compositions.
void ZoomScreenChanged();
// The frame to present: during a match, `ui` with the map composited into its keyed pixels (or `ui` itself).
// The first call after a match fills the keyed pixels of `ui` with the last map image, so the screens that
// follow never show the key.
const uint8_t* ZoomFrame(uint8_t* ui, bool inGame);
// F12: writes <base>_ui.bmp (key shown as magenta) and <base>_map.bmp, and logs the blended popups.
void DumpZoomBuffers(const std::wstring& base);

}  // namespace scpatch::features
