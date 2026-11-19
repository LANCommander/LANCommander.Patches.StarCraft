#pragma once

#include <windows.h>
#include <ddraw.h>

#include <cstdint>

#include "render/presenter.h"

// A minimal DirectDraw implementation for the 8-bit, single-primary-surface way StarCraft uses it (see
// re/notes/rendering.md, section 1). Surfaces are plain system-memory buffers; the primary surface hands each
// finished frame to the Direct3D presenter instead of a real display mode change.
namespace scpatch::render {

void SetPresenterOptions(const PresenterOptions& options);

// Makes the primary surface a view of another 8-bit buffer: Lock returns `*pixels` with pitch `width`, and
// Unlock/Blt call `present` instead of presenting the surface's own memory. Used by widescreen, where the
// game's screen buffer is wider than the 640x480 display mode the game asks for and some code (dialog
// fast-redraw, video playback) draws straight to the primary surface.
struct PrimaryRedirect {
  uint8_t* const* pixels = nullptr;  // address of the buffer pointer (may be null until allocated)
  int width = 0;
  int height = 0;
  void (*present)() = nullptr;
};
void SetPrimaryRedirect(const PrimaryRedirect& redirect);
HRESULT WINAPI EmulatedDirectDrawCreate(GUID* guid, LPDIRECTDRAW* out, IUnknown* outer);

}  // namespace scpatch::render
