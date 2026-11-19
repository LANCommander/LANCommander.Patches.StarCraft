// Rally mining (re/notes/rally.md): a worker trained at a building whose rally point is on a mineral field or on
// its own refinery starts harvesting it. Vanilla sends it there with a plain move (order 6), where it idles.
//
// FUN_00466f50 (new unit in EAX, building in ECX) applies the rally point: Follow (0x31) when the rally unit has
// the new unit's owner, else Move to the rally position. Its entry jumps to RallyStub, which first offers the unit
// to RallyHarvest; when that issues a harvest order the vanilla body is skipped.
//
// This changes the simulation, so every player must agree: single-player games follow [Rally] Mining (and the
// create-screen choice), multiplayer games the lobby rules, replays the rules recorded in them.
//
// Rally marker ([Rally] ShowFlag, display only): while the local player has buildings with a rally point selected,
// the yellow targeting cursor (TargY) is drawn at each rally point at the end of the map pass (MapLayerDraw's call
// to the drag-box drawer, 0x4BD619), into whatever the map draws into (the screen buffer, or zoom's map buffer),
// and a yellow dot on the minimap. The map pass only repaints dirty 16 px cells, so:
//   - the cells under a drawn marker are marked dirty before the next incremental pass (its call to
//     ScanFogChanges, 0x4BD5C7), which repaints terrain, sprites and fog there before it is drawn again (or not);
//   - the cells under a marker just drawn are marked dirty, so they are presented and the HUD above is redrawn;
//   - while any marker is shown the map layer runs every composition (layer flag 0x20, as zoom does).

#include <windows.h>

#include <algorithm>
#include <iterator>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/patch.h"
#include "core/patch_set.h"
#include "features/features.h"

namespace scpatch::features {
namespace {

constexpr uintptr_t kApplyRally = 0x00466F50;
constexpr uintptr_t kApplyRallyResume = kApplyRally + 5;  // jz after `test ecx, ecx; push esi; mov esi, eax`
constexpr uintptr_t kOrderTarget = 0x004752B0;  // unit in ESI, target in EAX, push order (ret 4)
constexpr uintptr_t kCanRally = 0x0047B2E0;     // unit in EAX: a building type that has a rally point
constexpr uintptr_t kMultiplayer = 0x0057F0B4;
constexpr uintptr_t kReplayActive = 0x006D0F14;
constexpr uintptr_t kRightClickAction = 0x00662098;  // units.dat, byte per unit type
constexpr uintptr_t kUnitFlags = 0x00664080;         // units.dat special ability flags, dword per unit type

constexpr uint8_t kOrderMoveToGas = 0x51;
constexpr uint8_t kOrderMoveToMinerals = 0x55;

patch::PatchSet g_set("rally");
bool g_available;
bool g_preferred = true;  // [Rally] Mining: single-player, and what this player asks for as host
uintptr_t g_resume = kApplyRallyResume, g_orderTarget = kOrderTarget, g_canRally = kCanRally;

template <typename T>
T Field(uint32_t unit, int offset) {
  return *reinterpret_cast<T*>(unit + offset);
}

__declspec(naked) uint32_t __stdcall CanRally(uint32_t /*building*/) {
  __asm {
    mov eax, [esp + 4]
    call dword ptr [g_canRally]
    ret 4
  }
}

__declspec(naked) void __stdcall OrderTarget(uint32_t /*unit*/, uint32_t /*target*/, uint32_t /*order*/) {
  __asm {
    push ebp
    mov ebp, esp
    push esi
    mov esi, [ebp + 8]
    mov eax, [ebp + 12]
    push dword ptr [ebp + 16]
    call dword ptr [g_orderTarget]
    pop esi
    pop ebp
    ret 12
  }
}

// The harvest order a right-click on `target` gives `worker` (FUN_004557c0, for a unit carrying nothing), or 0.
uint8_t HarvestOrder(uint32_t worker, uint32_t target) {
  uint16_t type = Field<uint16_t>(target, 0x64);
  if (*reinterpret_cast<uint32_t*>(kUnitFlags + type * 4) & 0x800) return 0;
  if (type >= 0xB0 && type <= 0xB2) return kOrderMoveToMinerals;  // mineral fields
  // FUN_00468930: a completed Refinery / Assimilator / Extractor of the worker's owner.
  bool gas = type == 0x6E || type == 0x9D || type == 0x95;
  if (gas && (Field<uint32_t>(target, 0xDC) & 1) && Field<uint8_t>(target, 0x4C) == Field<uint8_t>(worker, 0x4C))
    return kOrderMoveToGas;
  return 0;
}

// True if `unit`, just trained at `building`, was sent to harvest the rally target.
bool __cdecl RallyHarvest(uint32_t unit, uint32_t building) {
  if (!building || !unit || !RallyMiningActive() || !CanRally(building)) return false;
  uint32_t target = Field<uint32_t>(building, 0xFC);
  if (!target || target == building || !Field<int16_t>(building, 0xF8)) return false;
  // The rally unit is not cleared when it dies; its slot may hold another unit by now. The rally position is the
  // target's position when it was set (FUN_004560d0), and resources do not move.
  uint32_t sprite = Field<uint32_t>(target, 0x0C);
  if (!sprite || Field<int16_t>(sprite, 0x14) != Field<int16_t>(building, 0xF8) ||
      Field<int16_t>(sprite, 0x16) != Field<int16_t>(building, 0xFA))
    return false;
  // Workers: right-click action 4 (harvest) or 5 (harvest and repair); hallucinations cannot harvest (FUN_004560d0).
  uint8_t action = *reinterpret_cast<uint8_t*>(kRightClickAction + Field<uint16_t>(unit, 0x64));
  if ((action != 4 && action != 5) || (Field<uint32_t>(unit, 0xDC) & 0x40000000)) return false;
  uint8_t order = HarvestOrder(unit, target);
  if (!order) return false;
  OrderTarget(unit, target, order);
  return true;
}

// Replaces the first 5 bytes of FUN_00466f50 (test ecx, ecx; push esi; mov esi, eax).
__declspec(naked) void RallyStub() {
  __asm {
    pushad
    push ecx
    push eax
    call RallyHarvest
    add esp, 8
    test al, al
    popad  // flags survive
    jnz handled
    test ecx, ecx
    push esi
    mov esi, eax
    jmp dword ptr [g_resume]
  handled:
    ret
  }
}

// ---- Rally flag -----------------------------------------------------------------------------------------------

constexpr uintptr_t kScanFogCall = 0x004BD5C7;  // incremental map path: call ScanFogChanges
constexpr uintptr_t kScanFog = 0x004804D0;
constexpr uintptr_t kDragBoxCall = 0x004BD619;  // end of the map pass: call FUN_00470040 (drag box)
constexpr uintptr_t kDragBox = 0x00470040;
constexpr uintptr_t kScreen = 0x006CEFF0;         // {u16 w, u16 h, u8* pixels}: the map pass's target
constexpr uintptr_t kMapLayerFlags = 0x006CEFB5;  // g_layers[5].flags; 0x20 = run every composition
constexpr uintptr_t kVanillaGrid = 0x006CEFF8;    // 40 x 30 cells
constexpr uintptr_t kColors = 0x006CEB20;  // palette indices nearest to fixed colours (FUN_0041e450, 0x51A0F0)
constexpr uintptr_t kLocalPlayer = 0x00512684;
constexpr uintptr_t kInGameLoop = 0x006D11EC;
constexpr uintptr_t kScreenX = 0x0062848C, kScreenY = 0x006284A8;

// The marker is the yellow targeting cursor (cursor\TargY.grp, slot 2 of the cursor table loaded by
// FUN_004d1560, whose frame offsets it turns into pointers). GRP: u16 frame count, u16 w, u16 h, then 8 bytes per
// frame {u8 x, u8 y, u8 w, u8 h, data}; data = u16 row offsets, then per row: 0x80|n skip n, 0x40|n repeat the
// next byte n times, n copy n bytes. Placed like the cursor (FUN_004be120): top-left = point + frame x/y - 63.
constexpr uintptr_t kCursorTargY = 0x00596B28;
constexpr int kCursorHotspot = 0x3F;
constexpr DWORD kFrameMs = 100;  // animation step

// Minimap: units and pings are drawn into the surface 0x59C194 {u16 w, u16 h, u8* pixels} by FUN_004a4150
// (base), FUN_004a4ac0 (units) and DrawMinimap (pings), then the control is redrawn. A dot is added after the
// pings, at each DrawMinimap call. Map -> minimap: ((p << [0x59C1B0]) / [0x59CC68]) >> 5 (FUN_004a3fd0); the
// minimap is [0x59CC74] x [0x59CC64].
constexpr uintptr_t kDrawMinimap = 0x004A3870;
constexpr uintptr_t kDrawMinimapCalls[] = {0x004A520A, 0x004A5CE2, 0x004A5542, 0x004A4E19, 0x004A4EDE, 0x004A4FB2};
constexpr uintptr_t kMinimapSurface = 0x0059C194;
constexpr uintptr_t kMinimapShift = 0x0059C1B0, kMinimapScale = 0x0059CC68;
constexpr uintptr_t kMinimapW = 0x0059CC74, kMinimapH = 0x0059CC64;
constexpr int kColorYellow = 17, kColorBlack = 15;

bool g_showFlag;
std::vector<RECT> g_drawn;  // map rects (exclusive) of the markers drawn by the last map pass
bool g_forcedRun;           // we set the map layer's 0x20 flag

uint8_t Color(int index) { return *reinterpret_cast<uint8_t*>(kColors + index); }

// Rally points of the selected buildings the local player may see (its own; any in a replay).
std::vector<POINT> WantedFlags() {
  std::vector<POINT> flags;
  if (!g_showFlag || !*reinterpret_cast<uint8_t*>(kInGameLoop)) return flags;
  uint32_t units[24] = {};
  int n = SelectionUnits(units);
  bool replay = *reinterpret_cast<uint32_t*>(kReplayActive) != 0;
  uint8_t local = static_cast<uint8_t>(*reinterpret_cast<uint32_t*>(kLocalPlayer));
  for (int i = 0; i < n; ++i) {
    uint32_t unit = units[i];
    if (!unit || (!replay && Field<uint8_t>(unit, 0x4C) != local) || !CanRally(unit)) continue;
    // FUN_00468670 (shows the rally point on selection): none at (0, 0) or at the building itself.
    int16_t x = Field<int16_t>(unit, 0xF8), y = Field<int16_t>(unit, 0xFA);
    if ((!x && !y) || (x == Field<int16_t>(unit, 0x28) && y == Field<int16_t>(unit, 0x2A))) continue;
    bool dup = false;
    for (const POINT& p : flags) dup |= p.x == x && p.y == y;
    if (!dup) flags.push_back({x, y});
  }
  return flags;
}

// The map view inside the map pass's target (buffer coordinates).
void MapViewArea(int* w, int* h) {
  if (WidescreenActive()) {
    MapViewSize view = WidescreenMapView();
    *w = view.w;
    *h = view.h;
  } else {
    *w = 640;
    *h = 400;
  }
}

// Marks the screen-grid cells under map rect `r` (screen buffer only: while zoomed the map pass reads an
// all-dirty grid of its own, which zoom puts back afterwards).
void MarkDirty(const RECT& r) {
  int w, h;
  MapViewArea(&w, &h);
  h = (std::min)(h, WidescreenMapAreaHeight());
  int sx = *reinterpret_cast<int*>(kScreenX), sy = *reinterpret_cast<int*>(kScreenY);
  uint8_t* grid = WidescreenActive() ? WidescreenDirtyGrid() : reinterpret_cast<uint8_t*>(kVanillaGrid);
  int columns = WidescreenActive() ? WidescreenWidth() / 16 : 40;
  int x0 = (std::max)(static_cast<int>(r.left) - sx, 0), y0 = (std::max)(static_cast<int>(r.top) - sy, 0);
  int x1 = (std::min)({static_cast<int>(r.right) - sx, w, columns * 16}) - 1;
  int y1 = (std::min)(static_cast<int>(r.bottom) - sy, h) - 1;
  if (x0 > x1 || y0 > y1) return;
  for (int row = y0 / 16; row <= y1 / 16; ++row)
    for (int c = x0 / 16; c <= x1 / 16; ++c) grid[row * columns + c] = 1;
}

// Draws the current TargY frame centred on map point `p` into the map pass's target; returns its map rect.
bool DrawMarker(POINT p, RECT* drawn) {
  auto* grp = *reinterpret_cast<const uint8_t**>(kCursorTargY);
  auto* pixels = *reinterpret_cast<uint8_t**>(kScreen + 4);
  if (!grp || !pixels) return false;
  int frames = *reinterpret_cast<const uint16_t*>(grp) & 0x7FFF;
  if (!frames) return false;
  const uint8_t* frame = grp + 6 + 8 * ((GetTickCount() / kFrameMs) % frames);
  int fw = frame[2], fh = frame[3];
  auto* data = *reinterpret_cast<const uint8_t* const*>(frame + 4);
  int pitch = *reinterpret_cast<uint16_t*>(kScreen);
  int vw, vh;
  MapViewArea(&vw, &vh);
  vw = (std::min)(vw, pitch);
  int left = p.x + frame[0] - kCursorHotspot, top = p.y + frame[1] - kCursorHotspot;
  *drawn = {left, top, left + fw, top + fh};
  int bx = left - *reinterpret_cast<int*>(kScreenX), by = top - *reinterpret_cast<int*>(kScreenY);
  for (int row = 0; row < fh; ++row) {
    int y = by + row;
    if (y < 0 || y >= vh) continue;
    const uint8_t* src = data + reinterpret_cast<const uint16_t*>(data)[row];
    uint8_t* dst = pixels + static_cast<size_t>(y) * pitch;
    for (int col = 0; col < fw;) {
      uint8_t c = *src++;
      if (c & 0x80) {
        col += c & 0x7F;
        continue;
      }
      bool repeat = (c & 0x40) != 0;
      int n = repeat ? c & 0x3F : c;
      for (int i = 0; i < n && col < fw; ++i, ++col) {
        uint8_t v = repeat ? *src : src[i];
        int x = bx + col;
        if (x >= 0 && x < vw) dst[x] = v;
      }
      src += repeat ? 1 : n;
    }
  }
  return true;
}

// Before the incremental map pass: repaint what lies under the markers drawn last time.
void __cdecl FlagsBeforeMapPass() {
  for (const RECT& r : g_drawn) MarkDirty(r);
}

// End of the map pass, before the drag box: draw the markers.
void __cdecl FlagsAfterMapPass() {
  g_drawn.clear();
  for (POINT p : WantedFlags()) {
    RECT r;
    if (!DrawMarker(p, &r)) continue;
    MarkDirty(r);
    g_drawn.push_back(r);
  }
  uint8_t& flags = *reinterpret_cast<uint8_t*>(kMapLayerFlags);
  if (!g_drawn.empty() && !(flags & 0x20)) {
    flags |= 0x20;
    g_forcedRun = true;
  } else if (g_drawn.empty() && g_forcedRun) {
    flags &= ~0x20;  // zoom sets it again in every composition when it needs it
    g_forcedRun = false;
  }
}

// After the minimap's pings: a 3x3 yellow dot with a black outline at each rally point.
void __cdecl MinimapDots() {
  auto* surface = reinterpret_cast<uint8_t*>(kMinimapSurface);
  int pitch = *reinterpret_cast<uint16_t*>(surface);
  auto* pixels = *reinterpret_cast<uint8_t**>(surface + 4);
  int w = (std::min)(pitch, static_cast<int>(*reinterpret_cast<uint16_t*>(kMinimapW)));
  int h = (std::min)(*reinterpret_cast<uint16_t*>(surface + 2), *reinterpret_cast<uint16_t*>(kMinimapH));
  uint32_t scale = *reinterpret_cast<uint16_t*>(kMinimapScale);
  uint8_t shift = *reinterpret_cast<uint8_t*>(kMinimapShift) & 0x1F;
  if (!pixels || !scale) return;
  for (POINT p : WantedFlags()) {
    int mx = static_cast<int>(((static_cast<uint32_t>(p.x) << shift) / scale) >> 5);
    int my = static_cast<int>(((static_cast<uint32_t>(p.y) << shift) / scale) >> 5);
    for (int dy = -2; dy <= 2; ++dy) {
      for (int dx = -2; dx <= 2; ++dx) {
        int x = mx + dx, y = my + dy;
        if (x < 0 || y < 0 || x >= w || y >= h) continue;
        bool edge = dx == -2 || dx == 2 || dy == -2 || dy == 2;
        if (edge && (dx == dy || dx == -dy)) continue;  // rounded corners
        pixels[y * pitch + x] = Color(edge ? kColorBlack : kColorYellow);
      }
    }
  }
}

uintptr_t g_scanFog = kScanFog, g_dragBox = kDragBox, g_drawMinimap = kDrawMinimap;

__declspec(naked) void ScanFogStub() {
  __asm {
    pushad
    call FlagsBeforeMapPass
    popad
    jmp dword ptr [g_scanFog]
  }
}

__declspec(naked) void DragBoxStub() {
  __asm {
    pushad
    call FlagsAfterMapPass
    popad
    jmp dword ptr [g_dragBox]
  }
}

__declspec(naked) void DrawMinimapStub() {
  __asm {
    call dword ptr [g_drawMinimap]
    pushad
    call MinimapDots
    popad
    ret
  }
}

void InstallFlag() {
  if (!config::GetBool(L"Rally", L"ShowFlag", true)) return;
  if (!patch::RedirectCall(kScanFogCall, kScanFog, reinterpret_cast<void*>(&ScanFogStub), "rally flag: before map pass") ||
      !patch::RedirectCall(kDragBoxCall, kDragBox, reinterpret_cast<void*>(&DragBoxStub), "rally flag: draw")) {
    log::Error("rally: flag not installed");
    return;
  }
  g_showFlag = true;
  int minimap = 0;
  for (uintptr_t call : kDrawMinimapCalls)
    minimap += patch::RedirectCall(call, kDrawMinimap, reinterpret_cast<void*>(&DrawMinimapStub), "rally flag: minimap");
  log::Info("rally: marker shown at the rally points of selected buildings (minimap: %d of %zu draw calls)", minimap,
            std::size(kDrawMinimapCalls));
}

}  // namespace

bool RallyMiningAvailable() { return g_available; }
bool RallyMiningPreferred() { return g_available && g_preferred; }

void RallyMiningSetPreferred(bool enabled) {
  if (g_available && enabled != g_preferred) {
    g_preferred = enabled;
    log::Info("rally: mining %s (single-player and hosting)", enabled ? "enabled" : "disabled");
  }
}

bool RallyMiningActive() {
  if (!g_available) return false;
  if (*reinterpret_cast<uint32_t*>(kReplayActive)) return RulesReplayRallyMining();
  if (*reinterpret_cast<uint8_t*>(kMultiplayer)) return RulesRallyMining();
  return g_preferred;
}

void InstallRally() {
  InstallFlag();  // independent of the mining hook
  uint32_t rel = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&RallyStub) - (kApplyRally + 5));
  g_set.Add(kApplyRally, {0x85, 0xC9, 0x56, 0x8B, 0xF0},
            {0xE9, uint8_t(rel), uint8_t(rel >> 8), uint8_t(rel >> 16), uint8_t(rel >> 24)}, "apply rally point");
  if (!g_set.Verify()) {
    log::Error("rally: not installed");
    return;
  }
  // Installed even when off here: joining a game whose host enabled it needs it.
  g_set.Enable(true);
  g_preferred = config::GetBool(L"Rally", L"Mining", true);
  g_available = true;
  log::Info("rally: available; single-player and hosting mining %s", g_preferred ? "enabled" : "disabled");
}

}  // namespace scpatch::features
