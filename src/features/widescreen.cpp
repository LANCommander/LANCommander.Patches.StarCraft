// Widescreen: during a match the game renders W x 480 instead of 640 x 480; the map view widens to W x 400
// and the bottom HUD stays 640 wide, centred. Menus keep their 640 x 480 layout (the presenter shows only the
// left 640 columns outside a match). See re/notes/ws_*.md for the analysis behind every site.
//
// The screen buffer is W pixels wide at all times; everything that draws into it through the descriptor at
// 0x6CEFF0 follows automatically. The remaining hardcoded 640-based constants (about 200 sites: the dirty
// grid, the terrain ring cache, the fog lightmap, sprite culling, mouse clamps, ...) are rewritten once at
// startup from widescreen_sites.inc, each verified against its original bytes first. If any site does not
// match, nothing is patched and the game runs at 640 x 480.
//
// Constraints on W: the dirty grid (16 px cells) is indexed as row * 5 * 8 in several functions, which can
// only be rewritten in place as row * k * 8, so W/16 must be a multiple of 8 (W multiple of 128). Hence W is
// 768 (16:10) or 896 (~16:9) at the normal HUD size.
//
// HUD size: a smaller HUD is a taller screen, W x Hs with Hs = 480 + dy (dy a multiple of 32), widened to keep
// the aspect, with the bottom HUD moved down by dy. The map area above it becomes W x (400 + dy) (geometry g.H):
// without zoom the map view is that size (more map, drawn at the same pixel size as the HUD); with zoom the map
// keeps its on-screen scale. The size is chosen at startup ([Widescreen] HudSize, or the in-game menu's saved
// choice, which applies from the next start); menus keep their 640 x 480 layout in the top-left corner.

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/patch.h"
#include "core/patch_set.h"
#include "features/features.h"
#include "render/ddraw_emu.h"
#include "render/presenter.h"

namespace scpatch::features {
namespace {

// Derived dimensions. Vanilla values in brackets.
struct Geometry {
  int W;            // screen width                                      [640]
  int P;            // terrain ring pitch, W + 32                        [672]
  int M;            // terrain ring size, 448 * P                        [0x49800]
  int C;            // dirty grid cells per row, W / 16                  [40]
  int N;            // visible tile columns + 1, W / 32 + 1              [21]
  int T;            // visible tile columns, W / 32                      [20]
  int L;            // fog lightmap stride (8 px cells), W / 8 + 8       [88]
  int S;            // per-tile fog array stride, W / 32 + 4             [24]
  int gridDwords;   // dirty grid size in dwords, C * 30 / 4             [300]
  uint32_t grid;    // address of the relocated dirty grid               [0x6CEFF8]
  int dx;           // HUD offset, (W - 640) / 2                         [0]
  int dy;           // HUD offset down (HUD size), Hs - 480; g only      [0]
  int H;            // map view height (g: the screen's map area)        [400]
  int Hs;           // screen height around the view, H + 80             [480]
  int R;            // terrain ring rows, H + 48                         [448]
  int TH;           // visible tile rows (rounded up), (H + 31) / 32     [13]
  int fogBytes;     // per-tile fog array, S * (Hs/32 + 2) in dwords     [408]
  // The part of the view that is shown (the camera's view: scroll limits, centring, the minimap box, what is
  // "on screen"). Zoom renders one tile more than it shows on each axis so the presenter can place the shown
  // part with sub-8-px precision; otherwise the two are the same.
  int Wp;           // shown width                                       [640]
  int Hp;           // shown height                                      [400]
  int Tp;           // shown tile columns, Wp / 32                       [20]
  int THp;          // shown tile rows (rounded up), (Hp + 31) / 32      [13]
};

// Two geometries: g for the screen buffer, the HUD and the mouse (W x 480), m for the map view and everything
// drawn into it (terrain ring, fog, sprites, parallax, scrolling). They are the same unless zoom draws the map
// into a buffer of its own (features/zoom.cpp), which can be larger than the screen.
Geometry g;
Geometry m;
Geometry mx;  // the largest map view zoom or a HUD size can switch to: allocations are made for it
Geometry gx;  // the largest screen (the smallest HUD size): the screen buffer is allocated for it
// Dirty grid: C cells per row and 30 rows for the screen. While the map pass draws into a zoom buffer it reads
// the grid in map coordinates, with every cell dirty, so it is sized for the largest map view as well.
constexpr size_t kGridBytes = 16384;
uint8_t g_gridBuffer[kGridBytes];

void SetGeometry(Geometry& x, int W, int H) {
  x.W = W;
  x.P = W + 32;
  x.C = W / 16;
  x.N = W / 32 + 1;
  x.T = W / 32;
  x.L = W / 8 + 8;
  x.S = W / 32 + 4;
  x.H = H;
  x.Hs = H + 80;
  x.R = H + 48;
  x.M = x.R * x.P;
  x.TH = (H + 31) / 32;
  x.fogBytes = (x.S * (x.Hs / 32 + 2) + 3) & ~3;
  x.Wp = W;
  x.Hp = H;
  x.Tp = x.T;
  x.THp = x.TH;
}

// The screen for a HUD size: 480 + dy high, the normal width (g_baseWidth) widened in steps of 128 to keep the
// aspect, the HUD centred horizontally and moved down by dy.
int g_baseWidth = 640;

void SetScreen(Geometry& x, int dy) {
  int W = std::max(g_baseWidth, static_cast<int>(std::lround(g_baseWidth * (480.0 + dy) / 480 / 128)) * 128);
  SetGeometry(x, W, 400 + dy);
  x.gridDwords = x.C * (x.Hs / 16) / 4;
  x.grid = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_gridBuffer));
  x.dx = (W - 640) / 2;
  x.dy = dy;
}

// Zoom's map view: shows Wp x Hp, renders a tile more each way (the margin the sub-8-px offset moves into).
constexpr int kZoomMargin = 32;

void SetZoomGeometry(Geometry& x, int Wp, int Hp) {
  SetGeometry(x, Wp + kZoomMargin, Hp + kZoomMargin);
  x.Wp = Wp;
  x.Hp = Hp;
  x.Tp = Wp / 32;
  x.THp = (Hp + 31) / 32;
}

// Lightmap stride and fog painter row step for the caves below (map geometry).
int g_mapL;
int g_map8W;

constexpr uintptr_t kScreenPixels = 0x006CEFF4;  // SCR.pixels
constexpr uintptr_t kInGameLoop = 0x006D11EC;    // non-zero while the in-game loop runs
constexpr uintptr_t kCompose = 0x0041E280;
constexpr uintptr_t kComposeCall = 0x0041CB23;
constexpr uintptr_t kHudInit = 0x004C3BB0;
constexpr uintptr_t kHudInitCall = 0x004EEDAE;
constexpr uintptr_t kDialogFixup = 0x004194E0;
constexpr uintptr_t kConsoleSample = 0x004C362F;
constexpr uintptr_t kFullCopy = 0x0041D3A0;
constexpr uintptr_t kPresentRects = 0x0041D420;
constexpr uintptr_t kFlufTables = 0x005153E8;  // 4 pointers to {x, y, w, h, ptr} rows ending with x == -1

bool InGame() { return *reinterpret_cast<volatile uint8_t*>(kInGameLoop) != 0; }

bool g_reclip;

// ---- Presentation -------------------------------------------------------------------------------------------

// Replaces both of the game's copy-to-DirectDraw routines: the whole buffer goes to the presenter, which shows
// the full width during a match and the 640 x 480 menu area otherwise.
void __cdecl Present() {
  uint8_t* buffer = *reinterpret_cast<uint8_t* const*>(kScreenPixels);
  if (!buffer) return;
  const uint8_t* pixels = ZoomFrame(buffer, InGame());
  render::SubmitFrame(pixels, InGame() ? g.W : 640, InGame() ? g.Hs : 480, g.W);
  // After a HUD size change the cursor clip is rebuilt for the new screen, once the presenter has a frame of
  // that size to map it with: FUN_00421690 with EAX = 0 (the full screen; applied if the game clips at all).
  if (g_reclip) {
    g_reclip = false;
    __asm {
      xor eax, eax
      mov ecx, 0x00421690
      call ecx
    }
  }
}

int GridRows() { return g.Hs / 16; }  // dirty grid rows of the screen
int MapRows() { return g.H / 16; }    // ... of its map area

// ---- Fog consistency check (diagnostic, [Widescreen] FogCheck=1) -------------------------------------------
// After each composition, every 8x8 block of the map view whose four fog-lightmap corners are 0 (unexplored)
// must be black. The first violations are logged with the state from before the composition, to tell whether
// the map pass painted terrain without fog or something else wrote those pixels afterwards.

void __cdecl ComposeInner();

bool g_fogCheck;
int g_fogReports;
std::vector<uint8_t> g_gridBefore;
std::vector<uint8_t> g_screenBefore;  // frame before the composition, to see who wrote a bad block
std::vector<uint8_t> g_badLast;       // blocks that were bad after the previous composition
uint8_t g_mapFlagsBefore;
int g_cursorX, g_cursorY, g_prevCursorX, g_prevCursorY;
unsigned g_frame;

void CheckFog() {
  const uint8_t* pixels = *reinterpret_cast<uint8_t* const*>(kScreenPixels);
  const uint8_t* lightmap = *reinterpret_cast<uint8_t* const*>(0x006D5C18);
  if (!pixels || !lightmap) return;
  unsigned screenX = *reinterpret_cast<unsigned*>(0x0062848C);
  unsigned screenY = *reinterpret_cast<unsigned*>(0x006284A8);
  int bad = 0, reportedThisFrame = 0;
  int cols = g.W / 8;
  g_badLast.resize(static_cast<size_t>(cols) * (g.H / 8));
  for (int y = 0; y < g.H; y += 8) {
    for (int x = 0; x < g.W; x += 8) {
      uint8_t& wasBad = g_badLast[(y / 8) * cols + x / 8];
      int row = static_cast<int>((screenY >> 3) & 3) + (y >> 3) + 1;
      int col = static_cast<int>((screenX >> 3) & 3) + (x >> 3);
      const uint8_t* lm = lightmap + row * g.L + col;
      bool expectBlack = !(lm[-g.L] | lm[-g.L + 1] | lm[0] | lm[1]);
      bool nonBlack = false, changed = false;
      if (expectBlack) {
        for (int r = 0; r < 8; ++r) {
          const uint8_t* now = pixels + (y + r) * g.W + x;
          const uint8_t* before = g_screenBefore.data() + (y + r) * g.W + x;
          for (int c = 0; c < 8; ++c) {
            nonBlack |= now[c] != 0;
            changed |= now[c] != before[c];
          }
        }
      }
      bool isBad = expectBlack && nonBlack;
      bool isNew = isBad && !wasBad;
      wasBad = isBad;
      if (!isBad) continue;
      ++bad;
      // Only newly bad blocks are interesting; persistent ones (e.g. HUD frame pieces over fog) repeat forever.
      if (isNew && g_fogReports < 48 && reportedThisFrame < 4) {
        ++g_fogReports;
        ++reportedThisFrame;
        int cell = (y / 16) * g.C + x / 16;
        log::Warn("fogcheck: frame %u NEW bad block (%d,%d): pixels %s during compose; grid cell %s before; "
                  "map path %s; cursor %d,%d (prev %d,%d); view %u,%u",
                  g_frame, x, y, changed ? "CHANGED" : "unchanged (written outside compose)",
                  g_gridBefore[cell] ? "DIRTY" : "clean", (g_mapFlagsBefore & 1) ? "FULL" : "incremental",
                  g_cursorX, g_cursorY, g_prevCursorX, g_prevCursorY, screenX, screenY);
      }
    }
  }
  if (bad && (g_frame % 240) == 0) log::Warn("fogcheck: frame %u: %d bad block(s)", g_frame, bad);
}

// ---- Incremental map pass consistency ------------------------------------------------------------------------
// The incremental map pass repaints terrain only in dirty grid cells, then redraws every image that touches a
// dirty cell. Two invariants must hold or translucent images (shadows) darken the same pixels again every frame
// and moved sprites leave stale copies behind ([Widescreen] RepaintCheck=1 verifies them at runtime):
//   1. after PaintDirtyTerrain, every dirty cell holds exactly the terrain from the ring;
//   2. DrawSprites changes no pixel of a clean cell.
// The first violations of each are logged with their cell and grid value.

constexpr uintptr_t kPaintDirtyTerrain = 0x004BCDC0;
constexpr uintptr_t kPaintDirtyTerrainCall = 0x004BD5E6;
constexpr uintptr_t kDrawSprites = 0x00498D40;
constexpr uintptr_t kDrawSpritesCall = 0x004BD5EE;

bool g_repaintCheck;
int g_painterReports, g_spriteReports;
unsigned g_painterBadFrames, g_spriteBadFrames, g_checkedFrames;
std::vector<uint8_t> g_beforeSprites;

// The game redraws an image in full whenever any cell under it is dirty (FUN_00497ce0), but repaints terrain
// only in the dirty cells. Its clean cells keep the previous frame, so the image lands on top of whatever is
// there: an image behind another one comes out in front of it, shadows darken again every frame, and sprites
// cover HUD pieces that were not asked to redraw. Vanilla mostly hides this with slack in its marking. Close
// the dirty set instead: every image that will be drawn gets all of its cells marked, repeated until stable,
// so terrain, fog, sprites and the HUD all repaint exactly the same cells. Runs after MarkMovedSprites has
// refreshed every image's screen rect, over the same sprite rows.
void CloseDirtySet() {
  int first = *reinterpret_cast<int*>(0x005993A4);
  int last = *reinterpret_cast<int*>(0x005993C0);
  auto* const* rows = reinterpret_cast<uint8_t* const*>(0x00629688);  // sprites per map row, next at +4
  bool changed = true;
  for (int pass = 0; changed && pass < 16; ++pass) {
    changed = false;
    for (int row = first; row <= last; ++row) {
      for (uint8_t* sprite = rows[row]; sprite; sprite = *reinterpret_cast<uint8_t**>(sprite + 4)) {
        for (uint8_t* image = *reinterpret_cast<uint8_t**>(sprite + 0x1C); image;
             image = *reinterpret_cast<uint8_t**>(image + 4)) {
          uint16_t flags = *reinterpret_cast<uint16_t*>(image + 0xC);
          int x = *reinterpret_cast<int16_t*>(image + 0x20), y = *reinterpret_cast<int16_t*>(image + 0x22);
          int w = *reinterpret_cast<int16_t*>(image + 0x28), h = *reinterpret_cast<int16_t*>(image + 0x2A);
          if ((flags & 0x40) || w <= 0 || h <= 0) continue;  // hidden or off screen: never drawn
          // Same cell range as QueryRectDirty (0x497000).
          int c0 = std::clamp(x >> 4, 0, g.C - 1), c1 = std::clamp((x + w - 1) >> 4, 0, g.C - 1);
          int r0 = std::clamp(y >> 4, 0, MapRows() - 1), r1 = std::clamp((y + h - 1) >> 4, 0, MapRows() - 1);
          bool dirty = (flags & 1) != 0, clean = false;
          for (int r = r0; r <= r1; ++r) {
            for (int c = c0; c <= c1; ++c) (g_gridBuffer[r * g.C + c] ? dirty : clean) = true;
          }
          if (!dirty || !clean) continue;
          for (int r = r0; r <= r1; ++r) std::memset(g_gridBuffer + r * g.C + c0, 1, c1 - c0 + 1);
          changed = true;
        }
      }
    }
    // In-match popups (flag 0x10000000: game menu, options, ...) are blended onto the screen, and the dialog
    // layer redraws a whole popup as soon as any cell under it is dirty. Blending again over map cells that were
    // not repainted darkens them twice, so repaint the map under the whole popup.
    for (auto* d = *reinterpret_cast<uint8_t**>(0x006D5E34); d; d = *reinterpret_cast<uint8_t**>(d)) {
      uint32_t flags = *reinterpret_cast<uint32_t*>(d + 0x18);
      if (!(flags & 8) || !(flags & 0x10000000)) continue;
      int left = *reinterpret_cast<int16_t*>(d + 4), top = *reinterpret_cast<int16_t*>(d + 6);
      int right = *reinterpret_cast<int16_t*>(d + 8), bottom = *reinterpret_cast<int16_t*>(d + 10);
      if (top > g.H - 1 || right < 0 || left > g.W - 1) continue;
      int c0 = std::max(left, 0) >> 4, c1 = std::min(right, g.W - 1) >> 4;
      int r0 = std::max(top, 0) >> 4, r1 = std::min(bottom, g.H - 1) >> 4;
      bool dirty = false, clean = false;
      for (int r = r0; r <= r1; ++r) {
        for (int c = c0; c <= c1; ++c) (g_gridBuffer[r * g.C + c] ? dirty : clean) = true;
      }
      if (!dirty || !clean) continue;
      for (int r = r0; r <= r1; ++r) std::memset(g_gridBuffer + r * g.C + c0, 1, c1 - c0 + 1);
      changed = true;
    }
  }
}

// Replaces the incremental map path's call to PaintDirtyTerrain (0x4BD5E6).
void __cdecl ClosedPaintDirtyTerrain() {
  if (InGame()) CloseDirtySet();
  reinterpret_cast<void (*)()>(kPaintDirtyTerrain)();
  if (!g_repaintCheck || !InGame()) return;
  const uint8_t* pixels = *reinterpret_cast<uint8_t* const*>(kScreenPixels);
  const uint8_t* ring = *reinterpret_cast<uint8_t* const*>(0x00628454);
  if (!pixels || !ring) return;
  unsigned screenX = *reinterpret_cast<unsigned*>(0x0062848C);
  unsigned screenY = *reinterpret_cast<unsigned*>(0x006284A8);
  unsigned start = (screenY * g.P + screenX) % g.M;  // same origin as the painter (0x4BCDC6..0x4BCDF4)
  int bad = 0;
  for (int r = 0; r < MapRows(); ++r) {
    for (int c = 0; c < g.C; ++c) {
      uint8_t value = g_gridBuffer[r * g.C + c];
      if (!value) continue;
      int wrong = 0;
      for (int yy = 0; yy < 16; ++yy) {
        int y = r * 16 + yy;
        for (int xx = 0; xx < 16; ++xx) {
          int x = c * 16 + xx;
          wrong += pixels[y * g.W + x] != ring[(start + static_cast<unsigned>(y) * g.P + x) % g.M];
        }
      }
      if (!wrong) continue;
      ++bad;
      if (g_painterReports < 32) {
        ++g_painterReports;
        uint8_t left = c > 0 ? g_gridBuffer[r * g.C + c - 1] : 0xFF;
        log::Warn("repaint: frame %u cell (%d,%d) px (%d,%d) grid=%u left=%u: %d px differ from terrain after "
                  "PaintDirtyTerrain; view %u,%u",
                  g_checkedFrames, c, r, c * 16, r * 16, value, left, wrong, screenX, screenY);
      }
    }
  }
  if (bad) ++g_painterBadFrames;
}

void __cdecl CheckedDrawSprites() {
  const uint8_t* pixels = *reinterpret_cast<uint8_t* const*>(kScreenPixels);
  bool check = InGame() && pixels;
  if (check) g_beforeSprites.assign(pixels, pixels + static_cast<size_t>(g.W) * g.H);
  reinterpret_cast<void (*)()>(kDrawSprites)();
  if (!check) return;
  ++g_checkedFrames;
  int bad = 0;
  for (int r = 0; r < MapRows(); ++r) {
    for (int c = 0; c < g.C; ++c) {
      if (g_gridBuffer[r * g.C + c]) continue;
      int changed = 0;
      for (int yy = 0; yy < 16; ++yy) {
        size_t row = static_cast<size_t>(r * 16 + yy) * g.W + c * 16;
        changed += std::memcmp(pixels + row, g_beforeSprites.data() + row, 16) != 0;
      }
      if (!changed) continue;
      ++bad;
      if (g_spriteReports < 32) {
        ++g_spriteReports;
        log::Warn("repaint: frame %u clean cell (%d,%d) px (%d,%d): %d row(s) changed by DrawSprites (map path %s)",
                  g_checkedFrames, c, r, c * 16, r * 16, changed,
                  (*reinterpret_cast<uint8_t*>(0x006CEFB5) & 1) ? "FULL" : "incremental");
      }
    }
  }
  if (bad) ++g_spriteBadFrames;
  if ((g_checkedFrames % 600) == 0) {
    log::Info("repaint: %u frames checked; %u with painter misses, %u with sprites drawn on clean cells",
              g_checkedFrames, g_painterBadFrames, g_spriteBadFrames);
  }
}

// Adds a rectangle to the dialog layer's per-frame redraw region (FUN_0041c200 takes a RECT* in EAX).
void RequestDialogRedraw(RECT* rect) {
  __asm {
    mov eax, rect
    mov ecx, 0x0041C200
    call ecx
  }
}

// The map layer repaints dirty cells before the dialog layer runs, so any HUD dialog over a dirty cell must be
// redrawn on top or the map paints over it (most visible where the console's frame rises above y = 400, and
// after scrolling, which repaints the whole map). Request a redraw of every visible dialog touching a dirty cell.
// This must run after the map layer: moving sprites mark their cells during the map pass itself.
void RedrawDialogsOverDirtyCells() {
  for (auto* dialog = *reinterpret_cast<uint8_t**>(0x006D5E34); dialog; dialog = *reinterpret_cast<uint8_t**>(dialog)) {
    if (!(*reinterpret_cast<uint32_t*>(dialog + 0x18) & 8)) continue;  // not visible
    int left = *reinterpret_cast<int16_t*>(dialog + 4), top = *reinterpret_cast<int16_t*>(dialog + 6);
    int right = *reinterpret_cast<int16_t*>(dialog + 8), bottom = *reinterpret_cast<int16_t*>(dialog + 10);
    int c0 = std::max(left, 0) / 16, c1 = std::min(right, g.W - 1) / 16;
    int r0 = std::max(top, 0) / 16, r1 = std::min(bottom, g.Hs - 1) / 16;
    bool dirty = false;
    for (int r = r0; r <= r1 && !dirty; ++r) {
      for (int c = c0; c <= c1; ++c) {
        if (g_gridBuffer[r * g.C + c]) {
          dirty = true;
          break;
        }
      }
    }
    if (!dirty) continue;
    RECT rect{left, top, right, bottom};
    RequestDialogRedraw(&rect);
  }
}

// Replaces the dialog layer's first call (0x41CB59), the game's own check, which only covers in-game popups
// (flag 0x10000000). Layers run 7 -> 0, so the map layer (5) has painted and marked its cells by now, and the
// grid is only cleared after the last layer.
constexpr uintptr_t kGameDialogDirtyCheck = 0x0041C7B0;
constexpr uintptr_t kGameDialogDirtyCheckCall = 0x0041CB59;

void __cdecl DialogLayerDirtyCheck() {
  reinterpret_cast<void (*)()>(kGameDialogDirtyCheck)();
  if (InGame()) RedrawDialogsOverDirtyCells();
}

// Runs before each composition. The 80 px strips beside the centred HUD lie below the map area, so no layer
// ever paints them; clear them and mark them dirty so whatever crosses them (cursor, tooltips) redraws.
void ApplyPendingHudSize();

void __cdecl Compose() {
  ApplyPendingHudSize();  // between compositions, when no layer code is running
  bool check = g_fogCheck && InGame();
  if (check) {
    ++g_frame;
    g_gridBefore.assign(g_gridBuffer, g_gridBuffer + g.C * GridRows());
    g_mapFlagsBefore = *reinterpret_cast<uint8_t*>(0x006CEFB5);
    g_prevCursorX = g_cursorX;
    g_prevCursorY = g_cursorY;
    g_cursorX = *reinterpret_cast<int*>(0x006CDDC4);
    g_cursorY = *reinterpret_cast<int*>(0x006CDDC8);
    if (const uint8_t* pixels = *reinterpret_cast<uint8_t* const*>(kScreenPixels)) {
      g_screenBefore.assign(pixels, pixels + static_cast<size_t>(g.W) * g.Hs);
    } else {
      check = false;
    }
  }
  ComposeInner();
  if (check) CheckFog();
}

// The software cursor (layer 0) is only drawn when the cells under it are dirty, and is erased from the buffer
// after every present. The game presents only dirty cells, so the display keeps the cursor from earlier frames;
// the presenter shows the whole buffer, so frames where only other cells changed came out without a cursor.
// Flag 0x20 ("always run", survives ComposeLayers' `flags &= 0xF8`) draws it into every frame during a match.
constexpr uintptr_t kCursorLayerFlags = 0x006CEF51;  // g_layers[0].flags

void UpdateCursorLayer() {
  auto* flags = reinterpret_cast<volatile uint8_t*>(kCursorLayerFlags);
  *flags = InGame() ? (*flags | 0x20) : (*flags & ~0x20);
}

void __cdecl ComposeInner() {
  UpdateCursorLayer();
  if (InGame() && g.dx > 0) {
    uint8_t* pixels = *reinterpret_cast<uint8_t**>(kScreenPixels);
    if (pixels) {
      for (int y = g.H; y < g.Hs; ++y) {
        std::memset(pixels + y * g.W, 0, g.dx);
        std::memset(pixels + y * g.W + g.dx + 640, 0, g.W - g.dx - 640);
      }
    }
    for (int row = g.H / 16; row < g.Hs / 16; ++row) {
      std::memset(g_gridBuffer + row * g.C, 1, g.dx / 16);
      std::memset(g_gridBuffer + row * g.C + (g.dx + 640) / 16, 1, g.C - (g.dx + 640) / 16);
    }
  }
  reinterpret_cast<void (*)()>(kCompose)();
}

// ---- HUD centring -------------------------------------------------------------------------------------------

volatile uint8_t g_hudLoading;
int16_t g_dx16;
// Vertical shifts (HUD size): dialogs at the bottom (top >= 240: the console, minimap, command card, ...) move
// down by dy, ones at the top (top < 40: the resource bar) stay, anything else (in-match popups, laid out
// centred on 480) moves by dy / 2 to stay centred.
int16_t g_dy16, g_halfDy16;
int16_t g_sampleSplit16;  // HUD pieces at or below this y were moved down by dy (dy + 200)
uintptr_t g_fixupResume = kDialogFixup + 6;
uintptr_t g_consoleResume = kConsoleSample + 8;

// All in-game HUD dialogs are loaded inside FUN_004c3bb0; shift every dialog loaded there.
void __cdecl HudInit() {
  g_hudLoading = 1;
  reinterpret_cast<void (*)()>(kHudInit)();
  g_hudLoading = 0;
}

// Entry of the dialog fix-up routine (root dialog in EAX). Moving the root's x before the fix-up lets the game
// derive all dependent coordinates itself. Every dialog loaded for a match is shifted: the HUD (loaded inside
// FUN_004c3bb0) and anything opened while the match runs (game menu, options, objectives, ...), whose .bin
// layouts assume a 640-wide screen. Menus outside a match are untouched. Then runs the 6 displaced prologue
// bytes and resumes.
__declspec(naked) void DialogFixupStub() {
  __asm {
    cmp byte ptr [g_hudLoading], 0
    jne shift
    cmp byte ptr ds:[0x006D11EC], 0  // in-game loop flag
    je resume
  shift:
    test eax, eax
    je resume
    push ecx
    mov cx, word ptr [g_dx16]
    add word ptr [eax + 4], cx
    mov cx, word ptr [eax + 6]
    cmp cx, 240
    jge bottom
    cmp cx, 40
    jl done
    mov cx, word ptr [g_halfDy16]
    jmp move
  bottom:
    mov cx, word ptr [g_dy16]
  move:
    add word ptr [eax + 6], cx
  done:
    pop ecx
  resume:
    push ebp
    mov ebp, esp
    push esi
    mov esi, eax
    jmp dword ptr [g_fixupResume]
  }
}

// HUD pieces copy their artwork out of console.pcx at their own screen position; sample at the unshifted one.
__declspec(naked) void ConsoleSampleStub() {
  __asm {
    mov cx, word ptr [esi + 4]
    sub cx, word ptr [g_dx16]
    mov dx, word ptr [esi + 6]
    cmp dx, word ptr [g_sampleSplit16]
    jl resume
    sub dx, word ptr [g_dy16]
  resume:
    jmp dword ptr [g_consoleResume]
  }
}

// Minimap clicks (FUN_004a3d70, shared by view jumps, minimap orders and the view box) convert the cursor to
// minimap coordinates by subtracting the minimap control's x, which is relative to its panel; the panel sat at
// x = 0 in the original layout (y uses a hardcoded panel top instead). Also subtract the panel's x, reached
// through the control's root pointer (+0x32). ECX (the y pointer) is preserved, and the flags of the final
// `sub` feed the `jns` after the resume point.
constexpr uintptr_t kMinimapClickX = 0x004A3D77;
uintptr_t g_minimapClickResume = kMinimapClickX + 10;

__declspec(naked) void MinimapClickStub() {
  __asm {
    mov dx, word ptr [esi + 4]
    push ecx
    mov ecx, dword ptr [esi + 0x32]
    add dx, word ptr [ecx + 4]
    pop ecx
    sub word ptr [eax], dx
    mov dx, word ptr [eax]
    jmp dword ptr [g_minimapClickResume]
  }
}

// The minimap's screen rect at 0x512D00 (cursor clip while dragging on the minimap, minimap hit test) is rebuilt
// by FUN_004a4400 from the same control with the same omission: left = ctl->x, right = ctl->x + width - 1.
// Left: replaces `mov eax, [0x59cc60]; movsx ecx, word [eax+4]` (0x4A4526, 9 bytes) and adds the panel's x.
// Right: replaces `movsx ecx, word [eax+4]; movzx edx, word [0x59cc74]` (0x4A4545, 11 bytes) and starts from
// the corrected left instead.
constexpr uintptr_t kMinimapRectLeft = 0x004A4526;
constexpr uintptr_t kMinimapRectRight = 0x004A4545;
uintptr_t g_minimapRectLeftResume = kMinimapRectLeft + 9;
uintptr_t g_minimapRectRightResume = kMinimapRectRight + 11;

__declspec(naked) void MinimapRectLeftStub() {
  __asm {
    mov eax, dword ptr ds:[0x0059CC60]
    movsx ecx, word ptr [eax + 4]
    push edx
    mov edx, dword ptr [eax + 0x32]
    movsx edx, word ptr [edx + 4]
    add ecx, edx
    pop edx
    jmp dword ptr [g_minimapRectLeftResume]
  }
}

__declspec(naked) void MinimapRectRightStub() {
  __asm {
    mov ecx, dword ptr ds:[0x00512D00]
    movzx edx, word ptr ds:[0x0059CC74]
    jmp dword ptr [g_minimapRectRightResume]
  }
}

// ---- Fog lightmap stride caves -------------------------------------------------------------------------------
// FogPaint (FUN_004805f0) and ExpandFogLightmap (FUN_0047fe10) step through the lightmap with imm8/disp8
// strides. Each cave runs the displaced instructions with the stride read from g_mapL; flags are recomputed by
// the instruction at every resume point before anything tests them.
constexpr uintptr_t kLightRow = 0x00480617;     // imul ecx, ecx, L; shr ebx, 3
constexpr uintptr_t kLightFirst = 0x0048064C;   // add esi, L; dec eax; shr eax, 3
constexpr uintptr_t kLightAbove = 0x00480671;   // movzx edx, [esi - L]; movzx eax, [esi - L + 1] (loop head)
constexpr uintptr_t kLightNext = 0x004806CD;    // add esi, L; add ecx, 8 * W
constexpr uintptr_t kLightExpand = 0x0047FEAB;  // add edx, L; mov [ebp - 4], edx
uintptr_t g_lightRowResume = kLightRow + 6;
uintptr_t g_lightFirstResume = kLightFirst + 7;
uintptr_t g_lightAboveResume = kLightAbove + 8;
uintptr_t g_lightNextResume = kLightNext + 9;
uintptr_t g_lightExpandResume = kLightExpand + 6;

__declspec(naked) void LightRowStub() {
  __asm {
    imul ecx, dword ptr [g_mapL]
    shr ebx, 3
    jmp dword ptr [g_lightRowResume]
  }
}

__declspec(naked) void LightFirstStub() {
  __asm {
    add esi, dword ptr [g_mapL]
    dec eax
    shr eax, 3
    jmp dword ptr [g_lightFirstResume]
  }
}

__declspec(naked) void LightAboveStub() {
  __asm {
    mov edx, esi
    sub edx, dword ptr [g_mapL]
    movzx eax, byte ptr [edx + 1]
    movzx edx, byte ptr [edx]
    jmp dword ptr [g_lightAboveResume]
  }
}

__declspec(naked) void LightNextStub() {
  __asm {
    add esi, dword ptr [g_mapL]
    add ecx, dword ptr [g_map8W]
    jmp dword ptr [g_lightNextResume]
  }
}

__declspec(naked) void LightExpandStub() {
  __asm {
    add edx, dword ptr [g_mapL]
    mov dword ptr [ebp - 4], edx
    jmp dword ptr [g_lightExpandResume]
  }
}

// The console frame pieces ("statfluf") take their position from per-race tables after loading; shift the
// tables (all pieces sit at the bottom) by (dx, dy) more.
bool ShiftFlufTables(int dx, int dy) {
  int rows = 0;
  for (int race = 0; race < 4; ++race) {
    auto* row = *reinterpret_cast<int16_t**>(kFlufTables + race * 4);
    for (int i = 0; i < 64 && row[0] != -1; ++i, row += 6) {
      int16_t xy[2] = {static_cast<int16_t>(row[0] + dx), static_cast<int16_t>(row[1] + dy)};
      if (!patch::WriteMemory(reinterpret_cast<uintptr_t>(row), xy, sizeof(xy))) return false;
      ++rows;
    }
  }
  log::Info("widescreen: shifted %d HUD frame piece(s) by %d,%d px", rows, dx, dy);
  return true;
}

void SetHudGlobals() {
  g_dx16 = static_cast<int16_t>(g.dx);
  g_dy16 = static_cast<int16_t>(g.dy);
  g_halfDy16 = static_cast<int16_t>(g.dy / 2);
  g_sampleSplit16 = static_cast<int16_t>(g.dy + 200);
}

// Moves the dialogs of a running match from the `old` screen to the current one, by the same rule the fix-up
// placed them with (bottom: dy, top: none, in between: dy / 2). Roots hold absolute rects (+4..+A after
// DialogRegister); children are relative to them.
void MoveDialogs(const Geometry& old) {
  int ddx = g.dx - old.dx;
  int moved = 0;
  for (auto* d = *reinterpret_cast<uint8_t**>(0x006D5E34); d; d = *reinterpret_cast<uint8_t**>(d)) {
    auto* r = reinterpret_cast<int16_t*>(d + 4);  // left, top, right, bottom
    int ddy = r[1] >= 240 + old.dy ? g.dy - old.dy : r[1] < 40 ? 0 : g.dy / 2 - old.dy / 2;
    r[0] = static_cast<int16_t>(r[0] + ddx);
    r[2] = static_cast<int16_t>(r[2] + ddx);
    r[1] = static_cast<int16_t>(r[1] + ddy);
    r[3] = static_cast<int16_t>(r[3] + ddy);
    ++moved;
  }
  // The minimap's screen rect (cursor clip while dragging on it, its hit test), built when it was loaded.
  auto* minimap = reinterpret_cast<int*>(0x00512D00);
  minimap[0] += ddx;
  minimap[2] += ddx;
  minimap[1] += g.dy - old.dy;
  minimap[3] += g.dy - old.dy;
  log::Info("widescreen: moved %d dialog(s) for the new HUD size", moved);
}

// ---- Site table ---------------------------------------------------------------------------------------------

struct SiteDef {
  uintptr_t address;
  std::vector<uint8_t> original;
  int offset;
  int size;
  long long value;
  const char* what;
};

std::vector<SiteDef> GeneratedSites() {
  return {
#define SITE(address, bytes, offset, size, value, what) \
  {address, std::vector<uint8_t> bytes, offset, size, static_cast<long long>(value), what},
#include "features/widescreen_sites.inc"
#undef SITE
  };
}

bool Fits(const SiteDef& s) {
  return s.size == 4 || (s.size == 2 && s.value >= -32768 && s.value <= 65535) ||
         (s.size == 1 && s.value >= -128 && s.value <= 127);
}

// Every operand must fit for the current map geometry and for the largest one zoom may switch to (the
// values grow with the view, so those two bound all the levels in between).
// Likewise for the screen: the current one and the largest (the smallest HUD size).
bool SitesFit() {
  Geometry current = m, screen = g;
  bool ok = true;
  for (const Geometry& s : {screen, gx}) {
    g = s;
    for (const Geometry& view : {current, mx}) {
      m = view;
      for (const SiteDef& site : GeneratedSites()) {
        if (Fits(site)) continue;
        log::Error("widescreen: value %lld does not fit the %d-byte operand at %08X (%s; screen %dx%d, map view %dx%d)",
                   site.value, site.size, static_cast<unsigned>(site.address), site.what, g.W, g.Hs, m.W, m.H);
        ok = false;
      }
    }
  }
  m = current;
  g = screen;
  return ok;
}

void AddGenerated(patch::PatchSet& set) {
  for (const SiteDef& s : GeneratedSites()) {
    std::vector<uint8_t> replacement = s.original;
    uint32_t v = static_cast<uint32_t>(s.value);
    std::memcpy(replacement.data() + s.offset, &v, s.size);
    set.Add(s.address, s.original, replacement, s.what);
  }
}

// Hand-written sites whose imm32 follows the map geometry (see AddHandWritten): address of the operand.
struct MapOperand {
  uintptr_t address;
  int Geometry::*field;
};
constexpr MapOperand kMapOperands[] = {
    {0x0047EDD3 + 2, &Geometry::W},  // parallax row offset
    {0x0047EF3A + 2, &Geometry::W},  // parallax row offset
    {0x00480635 + 2, &Geometry::W},  // fog row offset
    {0x0040C26A + 7, &Geometry::P},  // ring blit start offset
};

// Shown map width for a shown height: the map area's aspect in whole tiles (the rendered view adds the margin).
int MapWidthForHeight(int H) {
  return std::clamp(static_cast<int>(static_cast<double>(g.W) * H / g.H / 32 + 0.5) * 32, 640, 2048 - kZoomMargin);
}

std::vector<uint8_t> Dword(uint32_t v) { return {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; }

std::vector<uint8_t> Concat(std::vector<uint8_t> a, const std::vector<uint8_t>& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

std::vector<uint8_t> JumpFrom(uintptr_t site, void* target, size_t length) {
  std::vector<uint8_t> code = Concat({0xE9}, Dword(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(target) - (site + 5))));
  code.resize(length, 0x90);
  return code;
}

std::vector<uint8_t> CallFrom(uintptr_t site, void* target) {
  return Concat({0xE8}, Dword(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(target) - (site + 5))));
}

struct HandSite {
  uintptr_t address;
  std::vector<uint8_t> original, replacement;
  const char* what;
};

// Hand-written sites that follow the screen geometry g; rewritten in place when the HUD size changes.
std::vector<HandSite> ScreenSites() {
  const uint8_t k = static_cast<uint8_t>(g.C / 8);
  std::vector<HandSite> sites = {
      // Dirty-grid row offsets computed as row * 5 (then * 8): lea r, [s + s*4] -> imul r, s, C/8.
      // Unlike LEA, IMUL clobbers the flags, so where a compare's flags are consumed after the LEA the compare is
      // moved after the IMUL (it reads none of the registers the IMUL writes).
      {0x0041DE4E, {0x8D, 0x04, 0x89}, {0x6B, 0xC1, k}, "dirty test row offset"},
      {0x0041E15B, {0x3B, 0xCE, 0x8D, 0x04, 0x89}, {0x6B, 0xC1, k, 0x3B, 0xCE},
       "dirty mark row offset (cmp ecx, esi kept last)"},
      {0x0042D2C7, {0x3B, 0xF8, 0x8D, 0x14, 0xBF}, {0x6B, 0xD7, k, 0x3B, 0xF8},
       "rect dirty row offset (cmp edi, eax kept last)"},
      {0x0047ECEB, {0x8D, 0x3C, 0x80}, {0x6B, 0xF8, k}, "map redraw row offset"},
      {0x0047ED3C, {0x8D, 0x1C, 0x9B}, {0x6B, 0xDB, k}, "map redraw row offset"},
      {0x0047ED5F, {0x8D, 0x04, 0x80}, {0x6B, 0xC0, k}, "map redraw row offset"},
      {0x00497060, {0x85, 0xC0, 0x8D, 0x0C, 0x89}, {0x6B, 0xC9, k, 0x85, 0xC0},
       "dirty query row offset (test eax, eax kept last)"},
      // Statically initialised screen rectangles in .data (invisible to code scans): the clip rect used for
      // dirty regions and dialog redraws {0, 0, 639, 479}, and a full-screen rect {0, 0, 640, 480}. HUD controls
      // outside them are never redrawn.
      {0x0051A164, {0x7F, 0x02, 0x00, 0x00}, Dword(g.W - 1), "screen clip rect right"},
      {0x0051A168, {0xDF, 0x01, 0x00, 0x00}, Dword(g.Hs - 1), "screen clip rect bottom"},
      {0x0051A174, {0x80, 0x02, 0x00, 0x00}, Dword(g.W), "screen rect right / width"},
      {0x0051A178, {0xE0, 0x01, 0x00, 0x00}, Dword(g.Hs), "screen rect bottom / height"},
      // Mouse hit region of the HUD's bottom-right panel {496, 354, 639, 479}; it moves with the HUD.
      {0x005136CC, {0xF0, 0x01, 0x00, 0x00}, Dword(496 + g.dx), "HUD panel hit region left"},
      {0x005136D0, {0x62, 0x01, 0x00, 0x00}, Dword(354 + g.dy), "HUD panel hit region top"},
      {0x005136D4, {0x7F, 0x02, 0x00, 0x00}, Dword(639 + g.dx), "HUD panel hit region right"},
      {0x005136D8, {0xDF, 0x01, 0x00, 0x00}, Dword(479 + g.dy), "HUD panel hit region bottom"},
  };
  // Message line 12's erase (FUN_0048cb80, and its copy in FUN_004b1fa0) marks whole grid rows 18..r as
  // (r - 17) * 40 bytes: lea ecx, [eax + eax*4 - 0x55]; shl ecx, 3 -> sub eax, 17; imul ecx, eax, C (EAX is
  // reloaded right after). The first row moves down with the HUD.
  const uint8_t eraseFirst = static_cast<uint8_t>(17 + g.dy / 16);
  for (uintptr_t site : {0x0048CBB5u, 0x004B2303u}) {
    sites.push_back({site, {0x8D, 0x4C, 0x80, 0xAB, 0xC1, 0xE1, 0x03},
                     {0x83, 0xE8, eraseFirst, 0x6B, 0xC8, static_cast<uint8_t>(g.C), 0x90}, "message line 12 erase size"});
  }
  // Terrain dirty-cell painter (FUN_004bcdc0) run length. The original measures a run of dirty cells by
  // re-testing the first cell, so it repaints one extra (clean) cell to the right of every run with raw
  // terrain. The fog pass covers only the dirty cells, which left unfogged cells trailing the cursor. Rewritten
  // to stop at the last dirty cell; it leaves the same state (esi/[ebp-8] = length, [ebp-4] = last cell).
  const uint8_t c8 = static_cast<uint8_t>(g.C);
  sites.push_back({0x004BCE0B,
                   {0x8D, 0x43, 0x01, 0x83, 0xF8, 0x28, 0xBE, 0x01, 0x00, 0x00, 0x00, 0x89, 0x75, 0xF8,
                    0x7D, 0x18, 0xEB, 0x03, 0x8D, 0x49, 0x00, 0x80, 0x39, 0x00, 0x74, 0x08, 0x41, 0x46,
                    0x40, 0x83, 0xF8, 0x28, 0x7C, 0xF3, 0x89, 0x75, 0xF8, 0x89, 0x4D, 0xFC},
                   {0xBE, 0x01, 0x00, 0x00, 0x00,  // mov esi, 1
                    0x8D, 0x43, 0x01,              // lea eax, [ebx + 1]
                    0x83, 0xF8, c8,                // loop: cmp eax, C
                    0x7D, 0x0B,                    //       jge done
                    0x80, 0x79, 0x01, 0x00,        //       cmp byte ptr [ecx + 1], 0
                    0x74, 0x05,                    //       je done
                    0x41, 0x46, 0x40,              //       inc ecx; inc esi; inc eax
                    0xEB, 0xF0,                    //       jmp loop
                    0x89, 0x75, 0xF8,              // done: mov [ebp - 8], esi
                    0x89, 0x4D, 0xFC,              //       mov [ebp - 4], ecx
                    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90},
                   "terrain painter run length"});
  return sites;
}

void AddHandWritten(patch::PatchSet& set) {
  for (const HandSite& s : ScreenSites()) set.Add(s.address, s.original, s.replacement, s.what);

  // Screen row offsets computed as row * 640 (lea r, [s + s*4]; shl r, 7) -> imul r, s, W. Map pass only.
  set.Add(0x0047EDD3, {0x8D, 0x0C, 0x89, 0xC1, 0xE1, 0x07}, Concat({0x69, 0xC9}, Dword(m.W)), "parallax row offset");
  set.Add(0x0047EF3A, {0x8D, 0x0C, 0xB6, 0xC1, 0xE1, 0x07}, Concat({0x69, 0xCE}, Dword(m.W)), "parallax row offset");
  set.Add(0x00480635, {0x8D, 0x0C, 0x92, 0xC1, 0xE1, 0x07}, Concat({0x69, 0xCA}, Dword(m.W)), "fog row offset");

  // Fog lightmap stride L as an imm8 (or disp8), which only holds L <= 127 (W <= 952): moved into caves that
  // read it from g_mapL, each with the instructions it displaces.
  set.Add(kLightRow, {0x6B, 0xC9, 0x58, 0xC1, 0xEB, 0x03}, JumpFrom(kLightRow, reinterpret_cast<void*>(&LightRowStub), 6),
          "fog painter lightmap row (cave)");
  set.Add(kLightFirst, {0x83, 0xC6, 0x58, 0x48, 0xC1, 0xE8, 0x03},
          JumpFrom(kLightFirst, reinterpret_cast<void*>(&LightFirstStub), 7), "fog painter first row (cave)");
  set.Add(kLightAbove, {0x0F, 0xB6, 0x56, 0xA8, 0x0F, 0xB6, 0x46, 0xA9},
          JumpFrom(kLightAbove, reinterpret_cast<void*>(&LightAboveStub), 8), "fog painter row above (cave)");
  set.Add(kLightNext, {0x83, 0xC6, 0x58, 0x81, 0xC1, 0x00, 0x14, 0x00, 0x00},
          JumpFrom(kLightNext, reinterpret_cast<void*>(&LightNextStub), 9), "fog painter next row (cave)");
  set.Add(kLightExpand, {0x83, 0xC2, 0x58, 0x89, 0x55, 0xFC},
          JumpFrom(kLightExpand, reinterpret_cast<void*>(&LightExpandStub), 6), "lightmap expand next row (cave)");

  // Terrain ring start offset Y * 672 built from shifts -> mov eax, [Y]; imul eax, eax, P.
  std::vector<uint8_t> ringStart = Concat(Concat({0xA1, 0xA8, 0x84, 0x62, 0x00, 0x69, 0xC0}, Dword(m.P)),
                                          std::vector<uint8_t>(19, 0x90));
  set.Add(0x0040C26A,
          {0xA1, 0xA8, 0x84, 0x62, 0x00, 0x8B, 0x1D, 0xA8, 0x84, 0x62, 0x00, 0xC1, 0xE0, 0x09, 0x8B,
           0x0D, 0xA8, 0x84, 0x62, 0x00, 0xC1, 0xE3, 0x07, 0xC1, 0xE1, 0x05, 0x03, 0xC3, 0x03, 0xC1},
          ringStart, "ring blit start offset");

  // Presentation, composition and HUD hooks.
  set.Add(kFullCopy, {0x55, 0x8B, 0xEC, 0x83, 0xEC}, JumpFrom(kFullCopy, reinterpret_cast<void*>(&Present), 5),
          "full copy -> presenter");
  set.Add(kPresentRects, {0x55, 0x8B, 0xEC, 0x83, 0xEC},
          JumpFrom(kPresentRects, reinterpret_cast<void*>(&Present), 5), "dirty copy -> presenter");
  set.Add(kComposeCall, CallFrom(kComposeCall, reinterpret_cast<void*>(kCompose)),
          CallFrom(kComposeCall, reinterpret_cast<void*>(&Compose)), "compose wrapper");
  set.Add(kGameDialogDirtyCheckCall, CallFrom(kGameDialogDirtyCheckCall, reinterpret_cast<void*>(kGameDialogDirtyCheck)),
          CallFrom(kGameDialogDirtyCheckCall, reinterpret_cast<void*>(&DialogLayerDirtyCheck)),
          "dialog layer: redraw HUD over dirty cells");
  set.Add(kPaintDirtyTerrainCall, CallFrom(kPaintDirtyTerrainCall, reinterpret_cast<void*>(kPaintDirtyTerrain)),
          CallFrom(kPaintDirtyTerrainCall, reinterpret_cast<void*>(&ClosedPaintDirtyTerrain)),
          "map pass: close dirty set before terrain");
  if (g_repaintCheck) {
    set.Add(kDrawSpritesCall, CallFrom(kDrawSpritesCall, reinterpret_cast<void*>(kDrawSprites)),
            CallFrom(kDrawSpritesCall, reinterpret_cast<void*>(&CheckedDrawSprites)), "repaint check: sprites");
  }
  {  // HUD placement (installed even with no offset: the HUD size can change during a match)
    set.Add(kHudInitCall, CallFrom(kHudInitCall, reinterpret_cast<void*>(kHudInit)),
            CallFrom(kHudInitCall, reinterpret_cast<void*>(&HudInit)), "HUD init wrapper");
    set.Add(kDialogFixup, {0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF0},
            JumpFrom(kDialogFixup, reinterpret_cast<void*>(&DialogFixupStub), 6), "dialog fix-up shift");
    set.Add(kConsoleSample, {0x66, 0x8B, 0x4E, 0x04, 0x66, 0x8B, 0x56, 0x06},
            JumpFrom(kConsoleSample, reinterpret_cast<void*>(&ConsoleSampleStub), 8), "console art sample");
    set.Add(kMinimapClickX, {0x66, 0x8B, 0x56, 0x04, 0x66, 0x29, 0x10, 0x66, 0x8B, 0x10},
            JumpFrom(kMinimapClickX, reinterpret_cast<void*>(&MinimapClickStub), 10), "minimap click x");
    set.Add(kMinimapRectLeft, {0xA1, 0x60, 0xCC, 0x59, 0x00, 0x0F, 0xBF, 0x48, 0x04},
            JumpFrom(kMinimapRectLeft, reinterpret_cast<void*>(&MinimapRectLeftStub), 9), "minimap screen rect left");
    set.Add(kMinimapRectRight, {0x0F, 0xBF, 0x48, 0x04, 0x0F, 0xB7, 0x15, 0x74, 0xCC, 0x59, 0x00},
            JumpFrom(kMinimapRectRight, reinterpret_cast<void*>(&MinimapRectRightStub), 11), "minimap screen rect right");
  }
}

// ---- HUD size ---------------------------------------------------------------------------------------------
// Offered as the screen's extra height dy (multiples of 32, so the map area stays 400 + 32n); the HUD's size
// relative to the screen is 480 / (480 + dy): 100, 88, 79, 71, 65 and 60 %. Saved like Game Speed (Storm
// registry, "Starcraft" key, value "HUD Size", in percent); [Widescreen] HudSize applies until a choice is saved.

constexpr int kHudExtra[] = {0, 64, 128, 192, 256, 320};
constexpr char kRegKey[] = "Starcraft";
constexpr char kHudRegValue[] = "HUD Size";
using SRegLoadStringFn = BOOL(WINAPI*)(const char*, const char*, uint32_t, char*, uint32_t);
using SRegSaveStringFn = BOOL(WINAPI*)(const char*, const char*, uint32_t, const char*);

int PercentForExtra(int dy) { return (48000 + (480 + dy) / 2) / (480 + dy); }

int ExtraForPercent(int percent) {
  int best = 0;
  for (int dy : kHudExtra)
    if (std::abs(PercentForExtra(dy) - percent) < std::abs(PercentForExtra(best) - percent)) best = dy;
  return best;
}

int SavedHudPercent() {
  auto load = reinterpret_cast<SRegLoadStringFn>(GetProcAddress(GetModuleHandleA("storm.dll"), MAKEINTRESOURCEA(422)));
  char saved[16] = {};
  if (!load || !load(kRegKey, kHudRegValue, 0, saved, sizeof(saved))) return 0;
  return atoi(saved);
}

bool g_hudAvailable;   // widescreen is installed: the HUD size can be chosen
int g_hudChosen = 100;  // percent chosen last
int g_pendingDy = -1;   // a chosen size not applied yet (applied before the next composition)

int ChooseWidth() {
  int requested = config::GetInt(L"Widescreen", L"Width", 0);
  if (requested == 640 || requested == 768 || requested == 896) return requested;
  if (requested != 0) log::Warn("widescreen: Width=%d unsupported (use 0, 640, 768 or 896); using auto", requested);
  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) || mode.dmPelsHeight == 0) return 640;
  double aspect = static_cast<double>(mode.dmPelsWidth) / mode.dmPelsHeight;
  return aspect >= 1.7 ? 896 : aspect >= 1.55 ? 768 : 640;
}

patch::PatchSet g_sites("widescreen");

}  // namespace

bool WidescreenActive() { return g_sites.Enabled(); }
int WidescreenWidth() { return g.W; }
int WidescreenScreenHeight() { return g_sites.Enabled() ? g.Hs : 480; }
int WidescreenMapAreaHeight() { return g_sites.Enabled() ? g.H : 400; }

bool HudSizeAvailable() { return g_hudAvailable; }
int HudSizeActive() { return g_sites.Enabled() ? PercentForExtra(g.dy) : 100; }
int HudSizeChosen() { return g_hudChosen; }

int HudSizeNext(int percent) {
  int dy = ExtraForPercent(percent);
  for (size_t i = 0; i < std::size(kHudExtra); ++i)
    if (kHudExtra[i] == dy) return PercentForExtra(kHudExtra[(i + 1) % std::size(kHudExtra)]);
  return 100;
}

void HudSizeChoose(int percent) {
  g_hudChosen = PercentForExtra(ExtraForPercent(percent));
  if (g_sites.Enabled()) g_pendingDy = ExtraForPercent(g_hudChosen);
  auto save = reinterpret_cast<SRegSaveStringFn>(GetProcAddress(GetModuleHandleA("storm.dll"), MAKEINTRESOURCEA(425)));
  char text[16];
  snprintf(text, sizeof(text), "%d", g_hudChosen);
  if (!save || !save(kRegKey, kHudRegValue, 0, text)) log::Error("widescreen: could not save the HUD size");
}
uint8_t* WidescreenDirtyGrid() { return g_gridBuffer; }
size_t WidescreenDirtyGridBytes() { return kGridBytes; }
MapViewSize WidescreenMapView() { return MapViewSize{m.W, m.H, m.Hs, m.Wp, m.Hp}; }
MapViewSize WidescreenLargestMapView() { return MapViewSize{mx.W, mx.H, mx.Hs, mx.Wp, mx.Hp}; }

namespace {

// Rewrites every generated operand that changed for the current g and m, then the hand-written ones.
void RewriteSites() {
  for (const SiteDef& s : GeneratedSites()) {
    uint32_t v = static_cast<uint32_t>(s.value);
    if (std::memcmp(reinterpret_cast<const void*>(s.address + s.offset), &v, s.size) != 0)
      patch::WriteMemory(s.address + s.offset, &v, s.size);
  }
  for (const HandSite& s : ScreenSites()) {
    if (std::memcmp(reinterpret_cast<const void*>(s.address), s.replacement.data(), s.replacement.size()) != 0)
      patch::WriteMemory(s.address, s.replacement.data(), s.replacement.size());
  }
  for (const MapOperand& op : kMapOperands) {
    int32_t v = m.*op.field;
    patch::WriteMemory(op.address, &v, sizeof(v));
  }
  g_mapL = m.L;
  g_map8W = 8 * m.W;
}

// Game state derived from the view size when the map loaded (InitMapView, the parallax setup).
void UpdateMapState() {
  int mapW = *reinterpret_cast<uint16_t*>(0x0057F1D4), mapH = *reinterpret_cast<uint16_t*>(0x0057F1D6);
  // Scroll limits are for the shown view (as InitMapView computes them), so at the right and bottom edges the
  // drawn view reaches a tile past the map: columns past the map are never refilled (RefillRingRow stops at the
  // map edge), and rows past it are drawn as the game already draws its own two rows past the bottom.
  *reinterpret_cast<int*>(0x00628488) = std::max(0, (mapW - m.Tp) * 32);                 // max scroll x
  *reinterpret_cast<int*>(0x006284B0) = std::max(0, (mapH - (m.Hp - 16) / 32) * 32 + 8);  // max scroll y
  int wrapX = (m.W + 8) * 256, wrapY = (m.Hs + 8) * 256;
  *reinterpret_cast<int*>(0x00628490) = wrapX;
  *reinterpret_cast<int*>(0x0062849C) = wrapY;
  auto rewrap = [](uintptr_t a, int wrap) {
    int& v = *reinterpret_cast<int*>(a);
    v %= wrap;
    if (v < 0) v += wrap;
  };
  for (int i = 0; i < 5; ++i) {  // parallax layer positions (1/256 px), kept inside the new wraps
    rewrap(0x00628474 + 4 * i, wrapX);
    rewrap(0x0062845C + 4 * i, wrapY);
  }
}

}  // namespace

bool WidescreenSetMapView(int H) {
  if (!g_sites.Enabled() || H < 400 || H > mx.Hp || (H - 400) % 32 != 0) return false;
  SetZoomGeometry(m, MapWidthForHeight(H), H);
  RewriteSites();
  UpdateMapState();
  return true;
}

namespace {

render::PrimaryRedirect g_redirect;

// Puts the camera so the view of the current size is centred on a map point, and refills the terrain ring for
// the new view (as zoom does after a view change).
void CentreCamera(double x, double y) {
  int left = static_cast<int>(std::floor((x - m.Wp / 2.0) / 8)) * 8;
  int top = static_cast<int>(std::floor((y - m.Hp / 2.0) / 8)) * 8;
  __asm {
    mov eax, left
    mov ecx, top
    mov edx, 0x0049C440  // SetScreenPosition: x in EAX, y in ECX; clamps and aligns to 8 px
    call edx
  }
  *reinterpret_cast<uint16_t*>(0x00628498) = *reinterpret_cast<uint16_t*>(0x0057F1D0);
  *reinterpret_cast<uint16_t*>(0x006284AC) = *reinterpret_cast<uint16_t*>(0x0057F1D2);
  reinterpret_cast<void (*)()>(0x0049BF20)();  // RefillRingView
}

// A HUD size chosen in the menu, applied between two compositions: the screen geometry and every site that
// follows it, the HUD and popups moved, the map view resized (without zoom it is the screen's map area; with
// zoom, zoom picks the view for the same map scale), and everything repainted.
void ApplyPendingHudSize() {
  if (g_pendingDy < 0) return;
  int dy = g_pendingDy;
  g_pendingDy = -1;
  Geometry old = g;
  SetScreen(g, dy);
  if (g.W == old.W && g.Hs == old.Hs) return;
  bool inGame = InGame(), split = ZoomSplitActive();
  double centreX = *reinterpret_cast<int*>(0x0062848C) + m.Wp / 2.0;
  double centreY = *reinterpret_cast<int*>(0x006284A8) + m.Hp / 2.0;
  SetHudGlobals();
  if (!split) {
    m = g;
    m.dy = 0;
  }
  RewriteSites();
  // Values the rewritten code stored when the game started or the match was set up: the screen descriptor
  // (w, h: the pitch everything draws with), the dialog layer's size (FUN_0041a030), and the map layer's size
  // and the map view rect (InitMapLayer 0x4BD630; screen coordinates, also with zoom).
  *reinterpret_cast<uint16_t*>(0x006CEFF0) = static_cast<uint16_t>(g.W);
  *reinterpret_cast<uint16_t*>(0x006CEFF2) = static_cast<uint16_t>(g.Hs);
  *reinterpret_cast<int16_t*>(0x006CEF7E) = static_cast<int16_t>(g.W);
  *reinterpret_cast<int16_t*>(0x006CEF80) = static_cast<int16_t>(g.Hs);
  *reinterpret_cast<int16_t*>(0x006CEFBA) = static_cast<int16_t>(g.W);
  *reinterpret_cast<int16_t*>(0x006CEFBC) = static_cast<int16_t>(g.H);
  SetRect(reinterpret_cast<RECT*>(0x005993B0), 0, 0, g.W - 1, g.H - 1);
  ShiftFlufTables(g.dx - old.dx, g.dy - old.dy);
  g_redirect.width = g.W;
  g_redirect.height = g.Hs;
  render::SetPrimaryRedirect(g_redirect);
  if (inGame) {
    MoveDialogs(old);
    int& cursorX = *reinterpret_cast<int*>(0x006CDDC4);
    int& cursorY = *reinterpret_cast<int*>(0x006CDDC8);
    cursorX = std::clamp(cursorX * g.W / old.W, 0, g.W - 1);
    cursorY = std::clamp(cursorY * g.Hs / old.Hs, 0, g.Hs - 1);
    // Repaint everything: the buffer's rows no longer line up with the new width.
    if (uint8_t* pixels = *reinterpret_cast<uint8_t**>(kScreenPixels))
      std::memset(pixels, 0, static_cast<size_t>(g.W) * g.Hs);
    std::memset(g_gridBuffer, 1, static_cast<size_t>(g.C) * GridRows());
    *reinterpret_cast<uint8_t*>(0x006CEFB5) |= 1;  // map layer: full redraw
    if (split) {
      ZoomScreenChanged();
    } else {
      UpdateMapState();
      CentreCamera(centreX, centreY);
    }
  }
  g_reclip = true;
  log::Info("widescreen: HUD size %d%%: %dx%d (HUD offset %d,%d px)", PercentForExtra(g.dy), g.W, g.Hs, g.dx, g.dy);
}

}  // namespace

// Debug dump for F12: geometry header, then the dirty grid (C x 30), the fog lightmap (L x Hs/8, 60 rows
// unzoomed) and the three per-tile fog arrays (fogBytes each, S x 17 unzoomed: new, old, raw).
void DumpWidescreenState(const wchar_t* path) {
  if (!g_sites.Enabled()) return;
  HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  char header[160];
  int n = snprintf(header, sizeof(header), "SCWS1 W=%d L=%d S=%d C=%d X=%d Y=%d mapW=%u mapH=%u ingame=%d\n", g.W, m.L,
                   m.S, g.C, *reinterpret_cast<int*>(0x0062848C), *reinterpret_cast<int*>(0x006284A8),
                   *reinterpret_cast<uint16_t*>(0x0057F1D4), *reinterpret_cast<uint16_t*>(0x0057F1D6), InGame());
  DWORD written;
  WriteFile(file, header, n, &written, nullptr);
  auto dump = [&](const void* p, size_t size) {
    std::vector<uint8_t> zero(size);
    WriteFile(file, p ? p : zero.data(), static_cast<DWORD>(size), &written, nullptr);
  };
  dump(g_gridBuffer, static_cast<size_t>(g.C) * GridRows());
  dump(*reinterpret_cast<uint8_t**>(0x006D5C18), static_cast<size_t>(m.L) * m.Hs / 8);
  for (uintptr_t a : {0x006D5C0Cu, 0x006D5C10u, 0x006D5C14u}) dump(*reinterpret_cast<uint8_t**>(a), m.fogBytes);
  CloseHandle(file);

  // Dialog list (root dialogs and their children): rects are absolute for roots, root-relative for children.
  int index = 0;
  for (auto* d = *reinterpret_cast<uint8_t**>(0x006D5E34); d && index < 64; d = *reinterpret_cast<uint8_t**>(d), ++index) {
    auto s16 = [](const uint8_t* p, int off) { return static_cast<int>(*reinterpret_cast<const int16_t*>(p + off)); };
    log::Info("dialog %2d %p rect (%d,%d)-(%d,%d) flags %08X type %d ctl %d", index, d, s16(d, 4), s16(d, 6),
              s16(d, 8), s16(d, 10), *reinterpret_cast<const uint32_t*>(d + 0x18), s16(d, 0x22), s16(d, 0x20));
    int child = 0;
    for (auto* c = *reinterpret_cast<uint8_t**>(d + 0x42); c && child < 64; c = *reinterpret_cast<uint8_t**>(c), ++child) {
      log::Info("    child %2d rect (%d,%d)-(%d,%d) flags %08X type %d", child, s16(c, 4), s16(c, 6), s16(c, 8),
                s16(c, 10), *reinterpret_cast<const uint32_t*>(c + 0x18), s16(c, 0x22));
    }
  }
  log::Info("widescreen: state dump %ls", path);
}

void InstallWidescreen() {
  if (!config::GetBool(L"Widescreen", L"Enabled", true)) {
    log::Info("widescreen: disabled in config");
    return;
  }
  if (!config::GetBool(L"Renderer", L"Enabled", true)) {
    log::Warn("widescreen: requires [Renderer] Enabled=1; not installed");
    return;
  }
  g_baseWidth = ChooseWidth();
  int saved = SavedHudPercent();
  g_hudChosen = PercentForExtra(ExtraForPercent(saved ? saved : config::GetInt(L"Widescreen", L"HudSize", 100)));
  if (config::GetInt(L"Widescreen", L"Width", 0) == 640) {
    log::Info("widescreen: Width=640; not installed (no HUD size either)");
    return;
  }
  g_hudAvailable = true;

  // The screen for the chosen HUD size, and the largest one (the smallest HUD), which the screen buffer and the
  // map-sized buffers are allocated for so the size can change during a match.
  SetScreen(gx, kHudExtra[std::size(kHudExtra) - 1]);
  SetScreen(g, ExtraForPercent(g_hudChosen));
  if (static_cast<size_t>(gx.C) * (gx.Hs / 16) > kGridBytes) {
    log::Error("widescreen: %dx%d needs a larger dirty grid; staying at 640x480", gx.W, gx.Hs);
    return;
  }
  SetHudGlobals();
  // Without zoom the map view is the screen's map area; the largest is the largest screen's.
  m = g;
  SetGeometry(mx, gx.W, gx.H);
  m.dy = mx.dy = 0;
  if (int startH, maxH; ZoomViewHeights(g.H, g.Hs, &startH, &maxH)) {
    // Zoom: the map view's height is 400 + 32n (keeping the view's half-tile offset), its width the map area's
    // aspect in whole tiles. Code is patched for the starting view; buffers are allocated for the largest view
    // of any HUD size.
    Geometry current = g;
    int largestW = 0, largestH = 0;
    for (int extra : kHudExtra) {
      SetScreen(g, extra);
      int s, l;
      ZoomViewHeights(g.H, g.Hs, &s, &l);
      largestW = std::max(largestW, MapWidthForHeight(l));
      largestH = std::max(largestH, l);
    }
    g = current;
    SetZoomGeometry(mx, largestW, largestH);
    SetZoomGeometry(m, MapWidthForHeight(startH), startH);
    log::Info("widescreen: zoom: map view %dx%d shown, %dx%d drawn (largest %dx%d)", m.Wp, m.Hp, m.W, m.H, mx.Wp,
              mx.Hp);
  }
  g_mapL = m.L;
  g_map8W = 8 * m.W;
  g_fogCheck = config::GetBool(L"Widescreen", L"FogCheck", false);
  g_repaintCheck = config::GetBool(L"Widescreen", L"RepaintCheck", false);
  if ((g_fogCheck || g_repaintCheck) && m.W != g.W) {
    log::Warn("widescreen: FogCheck/RepaintCheck assume the map is in the screen buffer; off while zoomed");
    g_fogCheck = g_repaintCheck = false;
  }

  if (!SitesFit()) return;
  AddGenerated(g_sites);
  AddHandWritten(g_sites);
  if (!g_sites.Verify()) {
    log::Error("widescreen: game code differs from what this patch expects; staying at 640x480");
    return;
  }
  if (!ShiftFlufTables(g.dx, g.dy)) {
    log::Error("widescreen: could not shift HUD frame pieces; staying at 640x480");
    return;
  }
  g_sites.Enable(true);
  // Code that draws straight to the DirectDraw primary (dialog fast redraws, video) must hit the same buffer.
  g_redirect.pixels = reinterpret_cast<uint8_t* const*>(kScreenPixels);
  g_redirect.width = g.W;
  g_redirect.height = g.Hs;
  // During a match the game redraws HUD controls straight to the primary in the middle of a composition, when
  // the software cursor is not in the buffer; presenting then shows frames without the cursor (flicker). Every
  // composition presents the whole buffer anyway, so those writes appear with the next frame.
  g_redirect.present = [] {
    if (!InGame()) Present();
  };
  render::SetPrimaryRedirect(g_redirect);
  log::Info("widescreen: %dx%d in matches (HUD size %d%%, HUD offset %d,%d px, %zu code sites; buffer for %dx%d)",
            g.W, g.Hs, PercentForExtra(g.dy), g.dx, g.dy, g_sites.Size(), gx.W, gx.Hs);
}

}  // namespace scpatch::features
