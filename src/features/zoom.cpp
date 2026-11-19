// Zoom: the map and UI are drawn into separate buffers during a match (re/notes/zoom_height.md).
//
// [Zoom] Enabled=1 (split): the compositor's map layers (rec5 map view, rec4/rec3 building placement) draw into
// a buffer of their own; the UI layers (rec2 dialogs/HUD, rec1 tooltip, rec0 cursor) keep drawing into the
// screen buffer (W x 480, or taller with a smaller HUD size), whose map area holds a key index wherever no UI is
// drawn. The presenter shows the UI with the map scaled into the keyed pixels of its map area (0..W x 0..400, the
// screen height - 80).
//   - Before the first UI layer of each composition, every dirty map cell of the screen buffer is reset: to the
//     key, or, under in-game popups (game menu, options, ...), which are blended onto what lies beneath them,
//     to the map pixels themselves (scaled to the screen) so the blend comes out the same.
//   - The dialog layer already redraws every dialog over a dirty cell (widescreen's RedrawDialogsOverDirtyCells),
//     the same rule that kept dialogs on top of the map before.
//   - The chat/message lines, which the game draws at the end of the map pass, are drawn in the UI pass instead.
// The key must be an index the UI layers never write: the probe found 1-13 (the tileset cycling slots) unused.
//
// Zoomed views: widescreen patches the map code for a view of Wv x Hv (map geometry m; widescreen.cpp) and
// the map buffer is Wv x (Hv + 80). During each map layer the screen descriptor is pointed at the map buffer
// with that size, and the GRP blitters' end-of-surface constant follows it. The map pass always takes its full
// path (layer flag 1) with every dirty-grid cell set: the grid is kept in screen coordinates for the UI, which
// the map's coordinates no longer match, so the map is simply redrawn whole.
//
// [Zoom] Level is the starting view, MinLevel the furthest the mouse wheel zooms out (Wheel=1). The game's
// map-sized buffers are allocated for the largest view, so a wheel step only repatches the map code
// (WidescreenSetMapView), moves the camera so the point under the cursor stays put, and refills the terrain.
// Steps are queued by the window procedure and applied at the start of the next composition, before the map
// pass, when no map code is running. In multiplayer games whose host set "Zooming: Disabled" (lobby rules), the
// view is held at 100% and the wheel goes to the game.
//
// [Zoom] Probe=1 (diagnostic): before and after every layer callback the screen buffer is compared with a
// snapshot, so each changed pixel is attributed to the layer that wrote it ("between" = writes made between
// callbacks, which in practice is the cursor restoring what it saved). Logs which palette indices each layer
// writes and which callbacks run. Pixels rewritten with the value they already had are not seen.
//
// Both hang off the compositor's per-layer callback call (FUN_0041e280, 0x41E39C).
#include <windows.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/patch_set.h"
#include "features/features.h"
#include "render/presenter.h"

namespace scpatch::features {
namespace {

constexpr uintptr_t kCallbackCall = 0x0041E39C;  // xor edx, edx; xor ecx, ecx; call [esi + 0xC]
constexpr uintptr_t kLayerTable = 0x006CEF50;    // 8 records of 20 bytes; ESI points at record + 4 here
constexpr uintptr_t kScreen = 0x006CEFF0;        // {u16 w, u16 h, u8* pixels}
constexpr uintptr_t kScreenPixels = kScreen + 4;
constexpr uintptr_t kDialogs = 0x006D5E34;       // root dialog list
constexpr uintptr_t kInGameLoop = 0x006D11EC;
constexpr int kMinViewHeight = 400;  // map views are 400 + 32n high
int g_areaH = 400;                   // the screen's map area (above the HUD; taller with a smaller HUD size)
int g_screenH = 480;

uintptr_t g_resume = kCallbackCall + 7;
patch::PatchSet g_hook("zoom");

bool InGame() { return *reinterpret_cast<volatile uint8_t*>(kInGameLoop) != 0; }

// Multiplayer games follow the host's "Zooming" lobby rule; single-player games and replays always zoom.
bool ZoomAllowed() {
  if (*reinterpret_cast<uint32_t*>(0x006D0F14)) return true;  // replay playing
  if (*reinterpret_cast<uint8_t*>(0x0057F0B4)) return RulesZoomAllowed();  // multiplayer
  return true;
}

int LayerIndex(uintptr_t esi) {
  int k = static_cast<int>((esi - 4 - kLayerTable) / 20);
  return k >= 0 && k <= 7 ? k : -1;
}

// ---- Split ---------------------------------------------------------------------------------------------------

constexpr uintptr_t kMapLayerFlags = 0x006CEFB5;  // g_layers[5].flags: 1 = full redraw, 0x20 = always run
constexpr uintptr_t kDrawMessages = 0x0048CF60;
constexpr uintptr_t kDrawMessagesCall = 0x004BD614;  // in MapLayerDraw, after the fog
// GRP blitters: `add/sub ebx, 640 * 480` (end of the target surface), imm32 at +2.
constexpr uintptr_t kBlitWraps[] = {0x40AFE6, 0x40B166, 0x40B250, 0x40B28C, 0x40B2E4, 0x40B3C8, 0x40B3FE,
                                    0x40B452, 0x40B5A7, 0x40BAAD, 0x40BAE9, 0x40BC2C, 0x40BC62};

bool g_split;
bool g_zoomed;  // the map is drawn with the zoom machinery (any view size, including the unzoomed one)
bool g_wheel;
int g_largestH;
int g_wheelSteps;  // queued: + zooms out, - zooms in
int g_wheelRest;   // wheel delta below one notch
int32_t g_sx = 0x10000, g_sy = 0x10000;  // screen -> map scale (16.16) for the input caves
int32_t g_ox16, g_oy16;                   // shown-view offset in the drawn view (16.16), added by the caves
int g_shownW, g_shownH;                   // shown part of the drawn map view
double g_offX, g_offY;                    // sub-8-px camera offset set by the last zoom step (map px)
double g_effX, g_effY;                    // the offset in use this frame (none where the view meets the map edge)
uint8_t g_key;
int g_W;                                   // screen width
int g_mapW, g_mapH, g_mapHs;               // map view and map buffer size
std::vector<uint8_t> g_map;                // map layers' buffer, mapW x mapHs (rows >= mapH unused)
uint32_t g_uiWrap, g_mapWrap;              // blitter end-of-surface sizes
uint8_t* g_savedPixels;                    // screen buffer while a map layer draws into g_map
uint32_t g_savedSize;                      // screen descriptor w/h while a map layer draws
std::vector<uint8_t> g_savedGrid;          // the screen's dirty grid while the map pass reads an all-dirty one
bool g_gridSaved;
int g_lastLayer = -1;
bool g_uiPrepared;
bool g_keyed;  // the screen buffer holds key pixels (cleared when they are filled in after a match)

bool IsMapLayer(int k) { return k >= 3 && k <= 5; }

void SetBlitWraps(uint32_t size) {
  for (uintptr_t a : kBlitWraps) *reinterpret_cast<uint32_t*>(a + 2) = size;
}

// Screen pixel (x, y) of the map area -> map buffer pixel.
inline const uint8_t* MapPixelAt(int x, int y) {
  int mx = static_cast<int>(g_effX) + x * g_shownW / g_W, my = static_cast<int>(g_effY) + y * g_shownH / g_areaH;
  return g_map.data() + static_cast<size_t>(my) * g_mapW + mx;
}

// The offset can only be used while the drawn view's extra tile lies inside the map: at the right and bottom
// scroll limits the shown view already ends at the map edge.
void UpdateViewOffset() {
  int maxX = *reinterpret_cast<int*>(0x00628488), maxY = *reinterpret_cast<int*>(0x006284B0);
  int screenX = *reinterpret_cast<int*>(0x0062848C), screenY = *reinterpret_cast<int*>(0x006284A8);
  g_effX = std::clamp(g_offX, 0.0, static_cast<double>(std::max(0, maxX - screenX)));
  g_effY = std::clamp(g_offY, 0.0, static_cast<double>(std::max(0, maxY - screenY)));
  g_ox16 = static_cast<int32_t>(g_effX * 65536);
  g_oy16 = static_cast<int32_t>(g_effY * 65536);
}

void DrawMessages() { reinterpret_cast<void (*)()>(kDrawMessages)(); }

// Replaces MapLayerDraw's call: while the map is drawn into its own buffer the messages go to the UI pass.
void __cdecl MapPassMessages() {
  if (!g_split || !InGame()) DrawMessages();
}

// Visible in-game popups that are blended onto the screen (flag 0x10000000), clipped to the map view. Rects
// are made exclusive (the dialog's right/bottom are inclusive).
int BlendedPopups(RECT* out, int max, bool clip = true) {
  int count = 0;
  for (auto* d = *reinterpret_cast<uint8_t**>(kDialogs); d && count < max; d = *reinterpret_cast<uint8_t**>(d)) {
    uint32_t flags = *reinterpret_cast<uint32_t*>(d + 0x18);
    if (!(flags & 8) || !(flags & 0x10000000)) continue;  // not visible, or not a blended popup
    RECT r{*reinterpret_cast<int16_t*>(d + 4), *reinterpret_cast<int16_t*>(d + 6),
           *reinterpret_cast<int16_t*>(d + 8) + 1, *reinterpret_cast<int16_t*>(d + 10) + 1};
    if (clip) {
      r.left = std::max<LONG>(r.left, 0);
      r.top = std::max<LONG>(r.top, 0);
      r.right = std::min<LONG>(r.right, g_W);
      r.bottom = std::min<LONG>(r.bottom, g_areaH);
      if (r.left >= r.right || r.top >= r.bottom) continue;
    }
    out[count++] = r;
  }
  return count;
}

// Resets the screen buffer's dirty map cells before the UI layers draw: key, or map pixels under blended popups.
void PrepareUiMapArea() {
  uint8_t* ui = *reinterpret_cast<uint8_t**>(kScreenPixels);
  uint8_t* grid = WidescreenDirtyGrid();
  if (!ui || !grid) return;
  const int C = g_W / 16;
  g_keyed = true;
  RECT popups[16];
  int popupCount = BlendedPopups(popups, 16);
  if (g_zoomed) {
    // The map under a popup changes every frame now (it is always redrawn); blend the popup again over it.
    for (int i = 0; i < popupCount; ++i) {
      const RECT& p = popups[i];
      for (int row = p.top / 16; row <= (p.bottom - 1) / 16; ++row)
        std::memset(grid + row * C + p.left / 16, 1, (p.right - 1) / 16 - p.left / 16 + 1);
    }
  }

  for (int row = 0; row < g_areaH / 16; ++row) {
    for (int col = 0; col < C; ++col) {
      if (!grid[row * C + col]) continue;
      int x0 = col * 16, y0 = row * 16;
      for (int y = y0; y < y0 + 16; ++y) std::memset(ui + static_cast<size_t>(y) * g_W + x0, g_key, 16);
      for (int i = 0; i < popupCount; ++i) {
        const RECT& p = popups[i];
        int left = std::max(x0, static_cast<int>(p.left)), right = std::min(x0 + 16, static_cast<int>(p.right));
        int top = std::max(y0, static_cast<int>(p.top)), bottom = std::min(y0 + 16, static_cast<int>(p.bottom));
        for (int y = top; y < bottom; ++y) {
          uint8_t* dst = ui + static_cast<size_t>(y) * g_W;
          for (int x = left; x < right; ++x) dst[x] = *MapPixelAt(x, y);
        }
      }
    }
  }
  DrawMessages();
}

// Map pass of a zoomed map: the whole grid reads dirty (map coordinates), and the screen's grid is kept aside.
void BeginZoomedMapPass() {
  if (g_gridSaved) return;
  uint8_t* grid = WidescreenDirtyGrid();
  g_savedGrid.assign(grid, grid + static_cast<size_t>(g_W / 16) * (g_screenH / 16));
  std::memset(grid, 1, WidescreenDirtyGridBytes());
  g_gridSaved = true;
}

void EndZoomedMapPass() {
  if (!g_gridSaved) return;
  uint8_t* grid = WidescreenDirtyGrid();
  std::memcpy(grid, g_savedGrid.data(), g_savedGrid.size());
  std::memset(grid + g_savedGrid.size(), 0, WidescreenDirtyGridBytes() - g_savedGrid.size());
  g_gridSaved = false;
}

void CallSetScreenPosition(int x, int y) {
  __asm {
    mov eax, x
    mov ecx, y
    mov edx, 0x0049C440  // SetScreenPosition: x in EAX, y in ECX; clamps and aligns to 8 px
    call edx
  }
}

void UseMapView(MapViewSize view) {
  g_mapW = view.w;
  g_mapH = view.h;
  g_mapHs = view.hs;
  g_shownW = view.pw;
  g_shownH = view.ph;
  g_mapWrap = static_cast<uint32_t>(g_mapW) * g_mapHs;
  g_sx = static_cast<int32_t>((static_cast<int64_t>(g_shownW) << 16) / g_W);
  g_sy = static_cast<int32_t>((static_cast<int64_t>(g_shownH) << 16) / g_areaH);
}

// The map point a run of wheel steps zooms around; a new one is taken when the cursor or the camera moved.
// The camera only takes multiples of 8 px (the fog overlay is on an 8 px grid), so it is set to the multiple of
// 8 below the exact position and the rest becomes the shown view's offset inside the drawn one (g_offX/Y).
struct ZoomAnchor {
  bool valid;
  int cx, cy;          // cursor (screen) when taken
  double worldX, worldY;
  int screenX, screenY;  // camera after the last step
};
ZoomAnchor g_anchor;
int g_startH;
int g_baseH = kMinViewHeight;  // the 100% view: the map at its normal on-screen scale

// View heights the wheel steps through: from 100% out to the largest view, each notch a constant ratio
// ([Zoom] WheelSpeed percent) rounded to the 32 px grid of view heights, at least one grid step.
std::vector<int> g_wheelLevels;

void BuildWheelLevels(int ratioPercent) {
  g_wheelLevels = {g_baseH};
  while (g_wheelLevels.back() < g_largestH) {
    int H = g_wheelLevels.back();
    double target = H * (100 + ratioPercent) / 100.0;
    int next = kMinViewHeight + 32 * static_cast<int>(std::floor((target - kMinViewHeight) / 32 + 0.5));
    g_wheelLevels.push_back(std::min(std::max(next, H + 32), g_largestH));
  }
}

void PlaceView(double x, double y);

// Changes the shown view to H px high, keeping the map point under the cursor in place (or the view centre:
// when the cursor is off the map view, or `centre`). Runs before the map pass of a composition.
void SetViewHeight(int H, bool centre) {
  if (H == g_shownH) return;
  int cx = *reinterpret_cast<int*>(0x006CDDC4), cy = *reinterpret_cast<int*>(0x006CDDC8);
  if (centre || cx < 0 || cx >= g_W || cy < 0 || cy >= g_areaH) {
    cx = g_W / 2;
    cy = g_areaH / 2;
  }
  int& screenX = *reinterpret_cast<int*>(0x0062848C);
  int& screenY = *reinterpret_cast<int*>(0x006284A8);
  UpdateViewOffset();
  if (!g_anchor.valid || cx != g_anchor.cx || cy != g_anchor.cy || screenX != g_anchor.screenX ||
      screenY != g_anchor.screenY) {
    g_anchor = ZoomAnchor{true, cx, cy, screenX + g_effX + (cx + 0.5) * g_shownW / g_W,
                          screenY + g_effY + (cy + 0.5) * g_shownH / g_areaH, 0, 0};
  }
  if (!WidescreenSetMapView(H)) return;
  UseMapView(WidescreenMapView());
  PlaceView(g_anchor.worldX - (cx + 0.5) * g_shownW / g_W, g_anchor.worldY - (cy + 0.5) * g_shownH / g_areaH);
}

// Puts the shown view's top-left at map point (x, y): the camera at the multiple of 8 below, the rest as the
// sub-8-px offset, then refills the terrain ring for the (possibly new) view size.
void PlaceView(double x, double y) {
  int& screenX = *reinterpret_cast<int*>(0x0062848C);
  int& screenY = *reinterpret_cast<int*>(0x006284A8);
  auto floor8 = [](double v) { return static_cast<int>(std::floor(v / 8)) * 8; };
  CallSetScreenPosition(floor8(x), floor8(y));
  g_offX = std::clamp(x - screenX, 0.0, 31.0);  // clamped to the map, the camera may be short of floor8(x)
  g_offY = std::clamp(y - screenY, 0.0, 31.0);
  g_anchor.screenX = screenX;
  g_anchor.screenY = screenY;
  // The terrain ring's layout follows the view size: refill all of it at the (possibly unchanged) position.
  *reinterpret_cast<uint16_t*>(0x00628498) = *reinterpret_cast<uint16_t*>(0x0057F1D0);
  *reinterpret_cast<uint16_t*>(0x006284AC) = *reinterpret_cast<uint16_t*>(0x0057F1D2);
  reinterpret_cast<void (*)()>(0x0049BF20)();  // RefillRingView
}

// Applies queued wheel notches, or holds the view at 100% while the lobby rules forbid zooming.
void ApplyPendingZoom() {
  if (!ZoomAllowed()) {
    g_wheelSteps = 0;
    SetViewHeight(g_baseH, true);
    return;
  }
  if (!g_wheelSteps) return;
  // The level nearest the current view (a starting Level may lie between two), moved by the notches.
  int index = 0;
  for (int i = 1; i < static_cast<int>(g_wheelLevels.size()); ++i)
    if (std::abs(g_wheelLevels[i] - g_shownH) < std::abs(g_wheelLevels[index] - g_shownH)) index = i;
  index = std::clamp(index + g_wheelSteps, 0, static_cast<int>(g_wheelLevels.size()) - 1);
  g_wheelSteps = 0;
  SetViewHeight(g_wheelLevels[index], false);
}

void SplitBefore(int k) {
  if (k >= g_lastLayer) g_uiPrepared = false;  // layers run 7 -> 0, so this is a new composition
  g_lastLayer = k;
  if (IsMapLayer(k)) {
    if (g_zoomed) {
      if (!g_gridSaved) {  // first map layer of this composition
        ApplyPendingZoom();
        UpdateViewOffset();
      }
      BeginZoomedMapPass();
      if (k == 5) *reinterpret_cast<uint8_t*>(kMapLayerFlags) |= 1;  // full path
      g_savedSize = *reinterpret_cast<uint32_t*>(kScreen);
      *reinterpret_cast<uint16_t*>(kScreen) = static_cast<uint16_t>(g_mapW);
      *reinterpret_cast<uint16_t*>(kScreen + 2) = static_cast<uint16_t>(g_mapHs);
      SetBlitWraps(g_mapWrap);
    }
    g_savedPixels = *reinterpret_cast<uint8_t**>(kScreenPixels);
    *reinterpret_cast<uint8_t**>(kScreenPixels) = g_map.data();
  } else if (!g_uiPrepared) {
    EndZoomedMapPass();
    PrepareUiMapArea();
    g_uiPrepared = true;
  }
}

void SplitAfter(int k) {
  if (IsMapLayer(k) && g_savedPixels) {
    *reinterpret_cast<uint8_t**>(kScreenPixels) = g_savedPixels;
    g_savedPixels = nullptr;
    if (g_zoomed) {
      *reinterpret_cast<uint32_t*>(kScreen) = g_savedSize;
      SetBlitWraps(g_uiWrap);
    }
  }
  // The map is redrawn in every composition while zoomed: its moving parts no longer mark screen cells the
  // compositor would test.
  if (g_zoomed && k == 2) *reinterpret_cast<uint8_t*>(kMapLayerFlags) |= 0x20;
}

// ---- Mouse on a zoomed map -----------------------------------------------------------------------------------
// The cursor and input events stay in screen coordinates (the HUD, dialogs and their hit tests use them). The
// map is reached by adding the scroll position to a cursor/event point in a handful of places; each of those
// scales the point to map-buffer coordinates first (offset + x * shownW / W, offset + y * shownH / area height, in
// 16.16; the offset is the shown view's sub-8-px position in the drawn one):
//   right click (smart order)  FUN_004564e0    event point + scroll
//   targeted order click       0x4bd500        event point + scroll
//   unit under the cursor      FUN_004d13e0    cursor + scroll (hover, cursor shape, click select)
//   drag-select release        0x46fea0        drag rect + scroll
//   building placement         ScreenToMapHitTest  cursor -> tile, tile -> map-buffer position of the box
//   drag box drawing           FUN_00470040    drag rect drawn into the map buffer

constexpr uintptr_t kRightClickPoint = 0x0045656A;
constexpr uintptr_t kOrderClickPoint = 0x004BD522;
constexpr uintptr_t kHoverPoint = 0x004D13E9;
constexpr uintptr_t kDragRelease = 0x0046FED7;
constexpr uintptr_t kPlacementPoint = 0x0048D677;
constexpr uintptr_t kDragDraw = 0x00470053;
constexpr uintptr_t kFillRect = 0x004E1C70;  // (x, w, h) on the stack, y in SI; colour in [0x6cf4ac]
uintptr_t g_rightClickResume = kRightClickPoint + 22;
uintptr_t g_orderClickResume = kOrderClickPoint + 12;
uintptr_t g_hoverResume = kHoverPoint + 27;
uintptr_t g_dragReleaseResume = kDragRelease + 5;
uintptr_t g_placementResume = kPlacementPoint + 5;
uintptr_t g_fillRect = kFillRect;

// movsx eax, [edi+0xe]; movsx edi, [edi+0x10]; mov esi, [screenY]; add eax, [screenX]; add edi, esi
__declspec(naked) void RightClickPointStub() {
  __asm {
    movsx eax, word ptr [edi + 0xE]
    movsx edi, word ptr [edi + 0x10]
    imul eax, dword ptr [g_sx]
    add eax, dword ptr [g_ox16]
    sar eax, 16
    imul edi, dword ptr [g_sy]
    add edi, dword ptr [g_oy16]
    sar edi, 16
    mov esi, dword ptr ds:[0x006284A8]
    add eax, dword ptr ds:[0x0062848C]
    add edi, esi
    jmp dword ptr [g_rightClickResume]
  }
}

// movsx esi, [edi+0xe]; movsx edi, [edi+0x10]; add edi, ecx (screenY); add esi, edx (screenX)
__declspec(naked) void OrderClickPointStub() {
  __asm {
    movsx esi, word ptr [edi + 0xE]
    movsx edi, word ptr [edi + 0x10]
    imul esi, dword ptr [g_sx]
    add esi, dword ptr [g_ox16]
    sar esi, 16
    imul edi, dword ptr [g_sy]
    add edi, dword ptr [g_oy16]
    sar edi, 16
    add edi, ecx
    add esi, edx
    jmp dword ptr [g_orderClickResume]
  }
}

// eax = screenX + cursorX, ecx = cursorY + screenY (edx = screenY)
__declspec(naked) void HoverPointStub() {
  __asm {
    mov eax, dword ptr ds:[0x006CDDC4]
    imul eax, dword ptr [g_sx]
    add eax, dword ptr [g_ox16]
    sar eax, 16
    add eax, dword ptr ds:[0x0062848C]
    mov ecx, dword ptr ds:[0x006CDDC8]
    imul ecx, dword ptr [g_sy]
    add ecx, dword ptr [g_oy16]
    sar ecx, 16
    mov edx, dword ptr ds:[0x006284A8]
    add ecx, edx
    jmp dword ptr [g_hoverResume]
  }
}

// The drag rect (words at 0x66ff50: x0, y0, x1, y1) is scaled in place, then the original adds the scroll.
__declspec(naked) void DragReleaseStub() {
  __asm {
    movsx eax, word ptr ds:[0x0066FF50]
    imul eax, dword ptr [g_sx]
    add eax, dword ptr [g_ox16]
    sar eax, 16
    mov word ptr ds:[0x0066FF50], ax
    movsx eax, word ptr ds:[0x0066FF54]
    imul eax, dword ptr [g_sx]
    add eax, dword ptr [g_ox16]
    sar eax, 16
    mov word ptr ds:[0x0066FF54], ax
    movsx eax, word ptr ds:[0x0066FF52]
    imul eax, dword ptr [g_sy]
    add eax, dword ptr [g_oy16]
    sar eax, 16
    mov word ptr ds:[0x0066FF52], ax
    movsx eax, word ptr ds:[0x0066FF56]
    imul eax, dword ptr [g_sy]
    add eax, dword ptr [g_oy16]
    sar eax, 16
    mov word ptr ds:[0x0066FF56], ax
    mov eax, dword ptr ds:[0x0062848C]
    jmp dword ptr [g_dragReleaseResume]
  }
}

// ax = x, cx = y (screen, already range-checked); sar ax, 5; push esi
__declspec(naked) void PlacementPointStub() {
  __asm {
    movsx eax, ax
    imul eax, dword ptr [g_sx]
    add eax, dword ptr [g_ox16]
    sar eax, 16
    movsx ecx, cx
    imul ecx, dword ptr [g_sy]
    add ecx, dword ptr [g_oy16]
    sar ecx, 16
    sar ax, 5
    push esi
    jmp dword ptr [g_placementResume]
  }
}

// Replaces the rest of FUN_00470040 once the box is known to be shown: fill (x0, y0, x1 - x0 + 1, y1 - y0 + 1)
// of the scaled rect.
__declspec(naked) void DragDrawStub() {
  __asm {
    mov al, byte ptr ds:[0x006CEB32]
    mov byte ptr ds:[0x006CF4AC], al
    push esi
    movsx esi, word ptr ds:[0x0066FF52]
    imul esi, dword ptr [g_sy]
    add esi, dword ptr [g_oy16]
    sar esi, 16
    movsx ecx, word ptr ds:[0x0066FF56]
    imul ecx, dword ptr [g_sy]
    add ecx, dword ptr [g_oy16]
    sar ecx, 16
    sub ecx, esi
    inc ecx
    push ecx
    movsx edx, word ptr ds:[0x0066FF54]
    imul edx, dword ptr [g_sx]
    add edx, dword ptr [g_ox16]
    sar edx, 16
    movsx eax, word ptr ds:[0x0066FF50]
    imul eax, dword ptr [g_sx]
    add eax, dword ptr [g_ox16]
    sar eax, 16
    sub edx, eax
    inc edx
    push edx
    push eax
    call dword ptr [g_fillRect]
    pop esi
    ret
  }
}

std::vector<uint8_t> JumpTo(uintptr_t site, void* target, size_t length) {
  std::vector<uint8_t> code(length, 0x90);
  code[0] = 0xE9;
  uint32_t rel = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(target) - (site + 5));
  std::memcpy(code.data() + 1, &rel, 4);
  return code;
}

void AddInputCaves(patch::PatchSet& set) {
  set.Add(kRightClickPoint,
          {0x0F, 0xBF, 0x47, 0x0E, 0x0F, 0xBF, 0x7F, 0x10, 0x8B, 0x35, 0xA8, 0x84, 0x62, 0x00, 0x03, 0x05, 0x8C, 0x84,
           0x62, 0x00, 0x03, 0xFE},
          JumpTo(kRightClickPoint, reinterpret_cast<void*>(&RightClickPointStub), 22), "right click map point");
  set.Add(kOrderClickPoint, {0x0F, 0xBF, 0x77, 0x0E, 0x0F, 0xBF, 0x7F, 0x10, 0x03, 0xF9, 0x03, 0xF2},
          JumpTo(kOrderClickPoint, reinterpret_cast<void*>(&OrderClickPointStub), 12), "targeted order map point");
  set.Add(kHoverPoint,
          {0xA1, 0x8C, 0x84, 0x62, 0x00, 0x8B, 0x0D, 0xC4, 0xDD, 0x6C, 0x00, 0x8B, 0x15, 0xA8, 0x84, 0x62, 0x00, 0x03,
           0xC1, 0x8B, 0x0D, 0xC8, 0xDD, 0x6C, 0x00, 0x03, 0xCA},
          JumpTo(kHoverPoint, reinterpret_cast<void*>(&HoverPointStub), 27), "unit under cursor map point");
  set.Add(kDragRelease, {0xA1, 0x8C, 0x84, 0x62, 0x00},
          JumpTo(kDragRelease, reinterpret_cast<void*>(&DragReleaseStub), 5), "drag select rect");
  set.Add(kPlacementPoint, {0x66, 0xC1, 0xF8, 0x05, 0x56},
          JumpTo(kPlacementPoint, reinterpret_cast<void*>(&PlacementPointStub), 5), "placement map point");
  set.Add(kDragDraw, {0xA0, 0x32, 0xEB, 0x6C, 0x00}, JumpTo(kDragDraw, reinterpret_cast<void*>(&DragDrawStub), 5),
          "drag box drawing");
}

// ---- Probe ---------------------------------------------------------------------------------------------------

constexpr int kBetween = 8;  // bucket for writes made between callbacks
constexpr int kBuckets = 9;
constexpr ULONGLONG kReportMs = 20000;  // short, so quitting straight from a match still leaves a recent report

struct LayerStats {
  uint64_t calls = 0;
  uint64_t callsWithWrites = 0;
  uint64_t mapPixels = 0;  // changed pixels in the map area (y < 400 at the normal HUD size)
  uint64_t hudPixels = 0;  // changed pixels below it
  int left = INT_MAX, top = INT_MAX, right = -1, bottom = -1;  // union of changed pixels
  uint64_t values[256] = {};
  uint32_t callbacks[8] = {};  // distinct callback addresses seen
};

bool g_probe;
LayerStats g_stats[kBuckets];
std::vector<uint8_t> g_snapshot;
int g_width, g_height;
bool g_wasInGame;
ULONGLONG g_lastReport;

// Attributes every pixel that differs from the snapshot to `bucket`, then brings the snapshot up to date.
void Diff(int bucket) {
  int w = *reinterpret_cast<int16_t*>(kScreen);
  int h = *reinterpret_cast<int16_t*>(kScreen + 2);
  const uint8_t* pixels = *reinterpret_cast<uint8_t* const*>(kScreenPixels);
  if (!pixels || w <= 0 || h <= 0) return;
  size_t size = static_cast<size_t>(w) * h;
  if (w != g_width || h != g_height) {  // first frame of a match, or a size change: nothing to compare yet
    g_snapshot.assign(pixels, pixels + size);
    g_width = w;
    g_height = h;
    return;
  }
  LayerStats& s = g_stats[bucket];
  bool wrote = false;
  for (int y = 0; y < h; ++y) {
    const uint8_t* now = pixels + static_cast<size_t>(y) * w;
    uint8_t* before = g_snapshot.data() + static_cast<size_t>(y) * w;
    if (std::memcmp(now, before, w) == 0) continue;
    for (int x = 0; x < w; ++x) {
      if (now[x] == before[x]) continue;
      ++s.values[now[x]];
      ++(y < g_areaH ? s.mapPixels : s.hudPixels);
      s.left = std::min(s.left, x);
      s.right = std::max(s.right, x);
      s.top = std::min(s.top, y);
      s.bottom = std::max(s.bottom, y);
      wrote = true;
    }
    std::memcpy(before, now, w);
  }
  if (wrote && bucket != kBetween) ++s.callsWithWrites;
}

void NoteCallback(int k) {
  const uint8_t* rec = reinterpret_cast<const uint8_t*>(kLayerTable + 20 * k);
  uint32_t callback = *reinterpret_cast<const uint32_t*>(rec + 0x10);
  LayerStats& s = g_stats[k];
  ++s.calls;
  for (uint32_t& slot : s.callbacks) {
    if (slot == callback) return;
    if (slot == 0) {
      slot = callback;
      log::Info("zoom probe: rec%d callback %08X (flags %02X, x %d, y %d, w %d, h %d, ctx %08X)", k, callback,
                rec[1], *reinterpret_cast<const int16_t*>(rec + 2), *reinterpret_cast<const int16_t*>(rec + 4),
                *reinterpret_cast<const int16_t*>(rec + 6), *reinterpret_cast<const int16_t*>(rec + 8),
                *reinterpret_cast<const uint32_t*>(rec + 0xC));
      return;
    }
  }
}

// Logs palette index ranges whose count in `values` is zero (or non-zero), in lines short enough for the log.
void LogIndexRanges(const char* label, const uint64_t* values, bool unused) {
  char line[900];
  int n = snprintf(line, sizeof(line), "zoom probe: %s:", label);
  int count = 0;
  for (int i = 0; i < 256;) {
    if ((values[i] == 0) != unused) {
      ++i;
      continue;
    }
    int j = i;
    while (j + 1 < 256 && (values[j + 1] == 0) == unused) ++j;
    count += j - i + 1;
    n += i == j ? snprintf(line + n, sizeof(line) - n, " %d", i) : snprintf(line + n, sizeof(line) - n, " %d-%d", i, j);
    if (n > 800) {
      log::Info("%s", line);
      n = snprintf(line, sizeof(line), "zoom probe: %s (cont.):", label);
    }
    i = j + 1;
  }
  snprintf(line + n, sizeof(line) - n, " (%d indices)", count);
  log::Info("%s", line);
}

void Report(const char* why) {
  log::Info("zoom probe: ---- report (%s), screen %dx%d ----", why, g_width, g_height);
  for (int k = 0; k < kBuckets; ++k) {
    const LayerStats& s = g_stats[k];
    char name[16];
    snprintf(name, sizeof(name), k == kBetween ? "between" : "rec%d", k);
    if (s.calls == 0 && s.mapPixels + s.hudPixels == 0) {
      log::Info("zoom probe: %s: never active", name);
      continue;
    }
    log::Info("zoom probe: %s: %llu calls, %llu with writes, changed px map %llu / hud %llu, area (%d,%d)-(%d,%d)",
              name, s.calls, s.callsWithWrites, s.mapPixels, s.hudPixels, s.left, s.top, s.right, s.bottom);
    // Most-written indices, for reading the UI's colour use at a glance.
    int order[256];
    for (int i = 0; i < 256; ++i) order[i] = i;
    std::partial_sort(order, order + 12, order + 256, [&](int a, int b) { return s.values[a] > s.values[b]; });
    char line[600];
    int n = snprintf(line, sizeof(line), "zoom probe: %s top indices:", name);
    for (int i = 0; i < 12 && s.values[order[i]]; ++i)
      n += snprintf(line + n, sizeof(line) - n, " %d=%llu", order[i], s.values[order[i]]);
    log::Info("%s", line);
    char label[32];
    snprintf(label, sizeof(label), "%s writes", name);
    LogIndexRanges(label, s.values, false);
  }
  // Candidate keys: indices never written by the UI layers (rec0 cursor, rec1 tooltip, rec2 dialogs/HUD).
  // "between" is left out: in practice it is the cursor restoring the pixels it saved (map pixels).
  uint64_t ui[256] = {};
  for (int k : {0, 1, 2})
    for (int i = 0; i < 256; ++i) ui[i] += g_stats[k].values[i];
  LogIndexRanges("indices never written by UI (rec0-2)", ui, true);
}

void ProbeBefore(int k) {
  Diff(kBetween);
  NoteCallback(k);
}

void ProbeAfter(int k, bool inGame) {
  if (inGame) {
    if (k >= 0) Diff(k);
    ULONGLONG now = GetTickCount64();
    if (!g_wasInGame) {
      g_lastReport = now;
      log::Info("zoom probe: match started, recording");
    } else if (now - g_lastReport >= kReportMs) {
      g_lastReport = now;
      Report("periodic");
    }
  } else if (g_wasInGame) {
    Report("match ended");
    for (LayerStats& s : g_stats) s = LayerStats{};
    g_width = g_height = 0;
  }
  g_wasInGame = inGame;
}

// ---- Hook ----------------------------------------------------------------------------------------------------

void __cdecl BeforeCallback(uintptr_t esi) {
  int k = LayerIndex(esi);
  if (k < 0 || !InGame()) return;
  if (g_probe) ProbeBefore(k);  // sees the screen buffer before the split swaps it out
  if (g_split) SplitBefore(k);
}

void __cdecl AfterCallback(uintptr_t esi) {
  int k = LayerIndex(esi);
  bool inGame = InGame();
  if (g_split && k >= 0) SplitAfter(k);  // restores the screen buffer even if the match just ended
  if (g_probe) ProbeAfter(k, inGame);
}

// Replaces the 7 bytes at 0x41E39C. The callback's two arguments are already on the stack, so it is called
// from here with the stack exactly as the original call had it.
__declspec(naked) void CallbackStub() {
  __asm {
    pushad
    push esi
    call BeforeCallback
    add esp, 4
    popad
    xor edx, edx
    xor ecx, ecx
    call dword ptr [esi + 0xC]
    pushad
    push esi
    call AfterCallback
    add esp, 4
    popad
    jmp dword ptr [g_resume]
  }
}

}  // namespace

namespace {

// View height for a zoom level in percent (100 = unzoomed, 50 = twice the view each way): 400 + 32n. Levels are
// relative to the map's normal on-screen scale: on the 480-high screen the 100% view is 400 high; on a taller
// screen (smaller HUD) its map area, areaH of screenH rows, shows areaH * 480 / screenH map rows at 100%.
int HeightForLevel(int level, int areaH, int screenH) {
  level = std::clamp(level, 50, 100);
  double rows = areaH * 48000.0 / screenH / level;  // at most 2 * 480 rows (level 50)
  return std::max(kMinViewHeight + 32 * static_cast<int>((rows - kMinViewHeight) / 32 + 0.5), kMinViewHeight);
}

}  // namespace

bool ZoomViewHeights(int areaH, int screenH, int* start, int* largest) {
  if (!config::GetBool(L"Zoom", L"Enabled", false)) return false;
  *start = HeightForLevel(config::GetInt(L"Zoom", L"Level", 100), areaH, screenH);
  *largest = *start;
  if (config::GetBool(L"Zoom", L"Wheel", true))
    *largest = std::max(*start, HeightForLevel(config::GetInt(L"Zoom", L"MinLevel", 50), areaH, screenH));
  return true;
}

namespace {
int g_wheelSpeed = 15;
}  // namespace

bool ZoomSplitActive() { return g_split; }

void ZoomScreenChanged() {
  if (!g_split) return;
  int& screenX = *reinterpret_cast<int*>(0x0062848C);
  int& screenY = *reinterpret_cast<int*>(0x006284A8);
  UpdateViewOffset();
  double scale = static_cast<double>(g_shownH) / g_baseH;  // the zoom level, kept
  double centreX = screenX + g_effX + g_shownW / 2.0, centreY = screenY + g_effY + g_shownH / 2.0;
  g_W = WidescreenWidth();
  g_areaH = WidescreenMapAreaHeight();
  g_screenH = WidescreenScreenHeight();
  g_uiWrap = *reinterpret_cast<uint32_t*>(kBlitWraps[0] + 2);  // widescreen's W * screen height, rewritten
  int start = g_startH, largest = g_largestH;
  ZoomViewHeights(g_areaH, g_screenH, &start, &largest);
  g_startH = start;
  g_baseH = std::min(HeightForLevel(100, g_areaH, g_screenH), start);
  g_largestH = std::min(largest, WidescreenLargestMapView().ph);
  g_wheel = g_largestH > g_baseH;
  BuildWheelLevels(g_wheelSpeed);
  int H = kMinViewHeight + 32 * static_cast<int>(std::floor((g_baseH * scale - kMinViewHeight) / 32 + 0.5));
  H = std::clamp(H, kMinViewHeight, g_largestH);
  g_anchor.valid = false;
  if (!WidescreenSetMapView(H)) {
    log::Error("zoom: no map view %d px high for the new screen", H);
    return;
  }
  UseMapView(WidescreenMapView());
  PlaceView(centreX - g_shownW / 2.0, centreY - g_shownH / 2.0);
  UpdateViewOffset();
  log::Info("zoom: screen %dx%d, map area %d px high, map view %dx%d", g_W, g_screenH, g_areaH, g_shownW, g_shownH);
}

bool ZoomWheel(int delta) {
  if (!g_split || !g_wheel || !InGame() || !ZoomAllowed()) return false;
  g_wheelRest += delta;
  int notches = g_wheelRest / WHEEL_DELTA;
  g_wheelRest -= notches * WHEEL_DELTA;
  g_wheelSteps -= notches;  // wheel forward zooms in
  return true;
}

const uint8_t* ZoomFrame(uint8_t* ui, bool inGame) {
  if (!g_split || g_map.empty()) return ui;
  if (!inGame) {
    render::SubmitMap(nullptr, 0, 0, 0, 0, RECT{}, render::MapSource{});
    // The end-of-match screens draw over this buffer piecemeal; until they have, it must not show the key.
    if (g_keyed) {
      for (int y = 0; y < g_areaH; ++y) {
        uint8_t* row = ui + static_cast<size_t>(y) * g_W;
        for (int x = 0; x < g_W; ++x)
          if (row[x] == g_key) row[x] = *MapPixelAt(x, y);
      }
      g_keyed = false;
    }
    // Every match starts at [Zoom] Level: set the view back before the next map is loaded and laid out for it.
    if (g_shownH != g_startH && WidescreenSetMapView(g_startH)) {
      UseMapView(WidescreenMapView());
      g_wheelSteps = 0;
      g_anchor.valid = false;
      g_offX = g_offY = g_effX = g_effY = 0;
      g_ox16 = g_oy16 = 0;
    }
    return ui;
  }
  UpdateViewOffset();
  render::SubmitMap(g_map.data(), g_mapW, g_mapH, g_mapW, g_key, RECT{0, 0, g_W, g_areaH},
                    render::MapSource{static_cast<float>(g_effX), static_cast<float>(g_effY),
                                      static_cast<float>(g_shownW), static_cast<float>(g_shownH)});
  return ui;
}

void DumpZoomBuffers(const std::wstring& base) {
  if (!g_split || g_map.empty() || !InGame()) return;
  const uint8_t* ui = *reinterpret_cast<uint8_t* const*>(kScreenPixels);
  if (!ui) return;
  bool okUi = render::SaveIndexed((base + L"_ui.bmp").c_str(), ui, g_W, g_screenH, g_W, g_key);
  bool okMap = render::SaveIndexed((base + L"_map.bmp").c_str(), g_map.data(), g_mapW, g_mapH, g_mapW);
  log::Info("zoom: F12 buffers %s: %ls_ui.bmp (key = magenta), %ls_map.bmp", okUi && okMap ? "saved" : "FAILED",
            base.c_str(), base.c_str());
  RECT popups[16];
  int n = BlendedPopups(popups, 16, false);
  for (int i = 0; i < n; ++i) {
    log::Info("zoom: blended popup %d: (%ld,%ld)-(%ld,%ld) exclusive", i, popups[i].left, popups[i].top,
              popups[i].right, popups[i].bottom);
  }
  // Every visible dialog, to spot a blended one whose drawing reaches outside its rect.
  for (auto* d = *reinterpret_cast<uint8_t**>(kDialogs); d; d = *reinterpret_cast<uint8_t**>(d)) {
    uint32_t flags = *reinterpret_cast<uint32_t*>(d + 0x18);
    if (!(flags & 8)) continue;
    log::Info("zoom: dialog %p (%d,%d)-(%d,%d) flags %08X", d, *reinterpret_cast<int16_t*>(d + 4),
              *reinterpret_cast<int16_t*>(d + 6), *reinterpret_cast<int16_t*>(d + 8),
              *reinterpret_cast<int16_t*>(d + 10), flags);
  }
}

void InstallZoom() {
  g_probe = config::GetBool(L"Zoom", L"Probe", false);
  g_split = config::GetBool(L"Zoom", L"Enabled", false);
  if (g_split && !WidescreenActive()) {
    log::Warn("zoom: requires widescreen (a wider-than-4:3 display or [Widescreen] Width); not enabled");
    g_split = false;
  }
  if (!g_probe && !g_split) return;
  g_areaH = WidescreenMapAreaHeight();
  g_screenH = WidescreenScreenHeight();
  if (g_split) {
    int key = config::GetInt(L"Zoom", L"KeyIndex", 1);
    if (key < 1 || key > 13) {
      log::Warn("zoom: KeyIndex=%d outside 1-13 (indices the UI never draws); using 1", key);
      key = 1;
    }
    g_key = static_cast<uint8_t>(key);
    g_W = WidescreenWidth();
    UseMapView(WidescreenMapView());
    g_startH = g_shownH;
    g_baseH = std::min(HeightForLevel(100, g_areaH, g_screenH), g_startH);
    MapViewSize largest = WidescreenLargestMapView();  // over every HUD size: the buffer is sized for it
    int startH = g_startH, largestH = largest.ph;
    ZoomViewHeights(g_areaH, g_screenH, &startH, &largestH);
    g_largestH = std::min(largestH, largest.ph);
    g_wheel = g_largestH > g_baseH;
    g_wheelSpeed = std::clamp(config::GetInt(L"Zoom", L"WheelSpeed", 15), 5, 50);
    BuildWheelLevels(g_wheelSpeed);
    if (g_wheel) {
      char levels[256];
      int n = 0;
      for (int H : g_wheelLevels) n += snprintf(levels + n, sizeof(levels) - n, " %d%%", 100 * g_baseH / H);
      log::Info("zoom: mouse wheel levels:%s", levels);
    }
    g_zoomed = true;
    g_map.assign(static_cast<size_t>(largest.w) * largest.hs, 0);
    g_uiWrap = *reinterpret_cast<uint32_t*>(kBlitWraps[0] + 2);  // widescreen's W * screen height
  }

  std::vector<uint8_t> jump(7, 0x90);
  jump[0] = 0xE9;
  uint32_t rel = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&CallbackStub) - (kCallbackCall + 5));
  std::memcpy(jump.data() + 1, &rel, 4);
  g_hook.Add(kCallbackCall, {0x33, 0xD2, 0x33, 0xC9, 0xFF, 0x56, 0x0C}, jump, "layer callback wrapper");
  if (g_split) {
    auto call = [](uintptr_t site, uintptr_t target) {
      uint32_t r = static_cast<uint32_t>(target - (site + 5));
      return std::vector<uint8_t>{0xE8, uint8_t(r), uint8_t(r >> 8), uint8_t(r >> 16), uint8_t(r >> 24)};
    };
    g_hook.Add(kDrawMessagesCall, call(kDrawMessagesCall, kDrawMessages),
               call(kDrawMessagesCall, reinterpret_cast<uintptr_t>(&MapPassMessages)), "messages -> UI pass");
  }
  if (g_zoomed) AddInputCaves(g_hook);
  if (!g_hook.Verify()) {
    if (g_zoomed) log::Error("zoom: the map code is patched for a zoomed view but its buffer cannot be hooked");
    g_probe = g_split = false;
    return;
  }
  g_hook.Enable(true);
  if (g_zoomed) {
    // The blitters' end-of-surface constants are rewritten on every switch between the two buffers.
    DWORD old;
    uintptr_t first = kBlitWraps[0], last = kBlitWraps[std::size(kBlitWraps) - 1] + 6;
    VirtualProtect(reinterpret_cast<void*>(first), last - first, PAGE_EXECUTE_READWRITE, &old);
  }
  if (g_split) {
    log::Info("zoom: map and UI drawn to separate buffers in matches (UI %dx%d, map %dx%d, key index %u%s)", g_W,
              g_screenH, g_mapW, g_mapH, g_key, g_wheel ? ", mouse wheel zooms" : "");
  }
  if (g_probe) log::Info("zoom probe: installed; reports every %llu s during matches and at match end", kReportMs / 1000);
}

}  // namespace scpatch::features
