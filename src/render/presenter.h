#pragma once

#include <windows.h>

#include <cstdint>

// Presents the game's 8-bit frame buffer through Direct3D 11 on a dedicated thread, scaled into the game
// window at the display's refresh rate. The game thread only hands over finished frames and palettes.
namespace scpatch::render {

enum class Scaling { Aspect, Integer, Stretch };
enum class Filter { Nearest, SharpBilinear };

struct PresenterOptions {
  Scaling scaling = Scaling::Aspect;
  Filter filter = Filter::SharpBilinear;
  bool vsync = true;
};

void StartPresenter(HWND window, const PresenterOptions& options);
void StopPresenter();

// Copies a frame (8-bit palette indices). Safe to call from the game thread at any rate.
void SubmitFrame(const uint8_t* pixels, int width, int height, int pitch);
// Updates `count` palette entries starting at `first`.
void SubmitPalette(const PALETTEENTRY* entries, int first, int count);

// A 32-bit layer drawn over the frame in the same game space (BGRA, one pixel per frame pixel; alpha 0 lets the
// frame show through), for the GDI dialog windows the game expects to sit on its fullscreen surface. nullptr
// removes it.
void SubmitOverlay(const uint32_t* bgra, int width, int height);
// A second 8-bit frame shown through the main one: wherever the main frame holds palette index `key` inside
// `area` (frame coordinates), the part `source` (x, y, w, h in map texels; x and y may be fractional) of the map
// frame is shown instead, scaled to `area` and sampled at the map's own resolution. pixels = nullptr turns it off.
struct MapSource {
  float x, y, w, h;
};
void SubmitMap(const uint8_t* pixels, int width, int height, int pitch, uint8_t key, RECT area, MapSource source);
// Called on the game thread at the start of every SubmitFrame with the frame's size, so the overlay can be
// refreshed in step with the frames.
void SetOverlayProvider(void (*provider)(int width, int height));

// True once the presenter has a window and a frame, so the mapping below means something.
bool FrameSubmitted();

// Mapping between the game's coordinate space (the frame buffer) and the window's client area.
POINT ClientToGame(POINT client);
POINT GameToClient(POINT game);
SIZE GameSize();

// Writes the last submitted frame, at its full size and with the current palette, as an 8-bit BMP.
bool SaveLastFrame(const wchar_t* path);
// Writes any 8-bit buffer as a BMP with the current palette; `highlight` (if >= 0) is shown as magenta.
bool SaveIndexed(const wchar_t* path, const uint8_t* pixels, int width, int height, int pitch, int highlight = -1);

}  // namespace scpatch::render
