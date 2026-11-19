#include <initguid.h>  // defines the IID_IDirectDraw* GUIDs declared by ddraw.h in this translation unit

#include "render/ddraw_emu.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "core/log.h"

namespace scpatch::render {
namespace {

PresenterOptions g_presenterOptions;
PrimaryRedirect g_redirect;

// Logs the first call to a method the game is not known to use, so gaps show up in scpatch.log.
#define UNSUPPORTED(name)                                                         \
  do {                                                                            \
    static bool logged;                                                           \
    if (!logged) log::Warn("ddraw: unsupported call %s", name), logged = true;    \
    return DDERR_UNSUPPORTED;                                                     \
  } while (0)

void FillPixelFormat(DDPIXELFORMAT& pf) {
  std::memset(&pf, 0, sizeof(pf));
  pf.dwSize = sizeof(pf);
  pf.dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
  pf.dwRGBBitCount = 8;
}

class Palette;
Palette* g_primaryPalette;  // palette attached to the primary surface; its changes are presented

class Palette final : public IDirectDrawPalette {
 public:
  explicit Palette(const PALETTEENTRY* initial) {
    if (initial) std::memcpy(entries_, initial, sizeof(entries_));
  }

  STDMETHODIMP QueryInterface(REFIID riid, LPVOID* out) override {
    if (riid == IID_IUnknown || riid == IID_IDirectDrawPalette) {
      AddRef();
      *out = this;
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
  STDMETHODIMP_(ULONG) Release() override {
    ULONG n = --refs_;
    if (n == 0) {
      if (g_primaryPalette == this) g_primaryPalette = nullptr;
      delete this;
    }
    return n;
  }

  STDMETHODIMP GetCaps(LPDWORD caps) override {
    *caps = DDPCAPS_8BIT | DDPCAPS_ALLOW256;
    return DD_OK;
  }
  STDMETHODIMP GetEntries(DWORD, DWORD start, DWORD count, LPPALETTEENTRY out) override {
    if (start >= 256 || !out) return DDERR_INVALIDPARAMS;
    count = std::min<DWORD>(count, 256 - start);
    std::memcpy(out, entries_ + start, count * sizeof(PALETTEENTRY));
    return DD_OK;
  }
  STDMETHODIMP Initialize(LPDIRECTDRAW, DWORD, LPPALETTEENTRY) override { return DDERR_ALREADYINITIALIZED; }
  STDMETHODIMP SetEntries(DWORD, DWORD start, DWORD count, LPPALETTEENTRY in) override {
    if (start >= 256 || !in) return DDERR_INVALIDPARAMS;
    count = std::min<DWORD>(count, 256 - start);
    std::memcpy(entries_ + start, in, count * sizeof(PALETTEENTRY));
    if (g_primaryPalette == this) SubmitPalette(entries_ + start, start, count);
    return DD_OK;
  }

  void SubmitAll() { SubmitPalette(entries_, 0, 256); }

 private:
  ULONG refs_ = 1;
  PALETTEENTRY entries_[256] = {};
};

class Surface final : public IDirectDrawSurface {
 public:
  Surface(int width, int height, bool primary)
      : width_(width), height_(height), pitch_((width + 3) & ~3), primary_(primary),
        pixels_(static_cast<size_t>(pitch_) * height) {}

  bool IsPrimary() const { return primary_; }

  // IUnknown
  STDMETHODIMP QueryInterface(REFIID riid, LPVOID* out) override {
    if (riid == IID_IUnknown || riid == IID_IDirectDrawSurface) {
      AddRef();
      *out = this;
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
  STDMETHODIMP_(ULONG) Release() override {
    ULONG n = --refs_;
    if (n == 0) {
      if (palette_) palette_->Release();
      delete this;
    }
    return n;
  }

  // Used by the game
  STDMETHODIMP Lock(LPRECT rect, LPDDSURFACEDESC desc, DWORD, HANDLE) override {
    if (!desc || desc->dwSize < sizeof(DDSURFACEDESC)) return DDERR_INVALIDPARAMS;
    Describe(*desc);
    uint8_t* p = Base();
    if (rect) p += rect->top * Pitch() + rect->left;
    desc->lpSurface = p;
    return DD_OK;
  }
  STDMETHODIMP Unlock(LPVOID) override {
    if (primary_) Present();
    return DD_OK;
  }
  STDMETHODIMP SetPalette(LPDIRECTDRAWPALETTE palette) override {
    if (palette) palette->AddRef();
    if (palette_) palette_->Release();
    palette_ = static_cast<Palette*>(palette);
    if (primary_) {
      g_primaryPalette = palette_;
      if (palette_) palette_->SubmitAll();
    }
    return DD_OK;
  }
  STDMETHODIMP GetPalette(LPDIRECTDRAWPALETTE* out) override {
    if (!palette_) return DDERR_NOPALETTEATTACHED;
    palette_->AddRef();
    *out = palette_;
    return DD_OK;
  }
  STDMETHODIMP Blt(LPRECT dstRect, LPDIRECTDRAWSURFACE src, LPRECT srcRect, DWORD flags, LPDDBLTFX fx) override {
    RECT dst = dstRect ? *dstRect : RECT{0, 0, Width(), Height()};
    if (!Clip(dst)) return DD_OK;
    if (flags & DDBLT_COLORFILL) {
      uint8_t color = fx ? static_cast<uint8_t>(fx->dwFillColor) : 0;
      for (LONG y = dst.top; y < dst.bottom; ++y) std::memset(Row(y) + dst.left, color, dst.right - dst.left);
    } else if (src) {
      auto* s = static_cast<Surface*>(src);
      RECT from = srcRect ? *srcRect : RECT{0, 0, s->Width(), s->Height()};
      CopyScaled(*s, from, dst);
    } else {
      return DDERR_INVALIDPARAMS;
    }
    if (primary_) Present();
    return DD_OK;
  }
  STDMETHODIMP BltFast(DWORD x, DWORD y, LPDIRECTDRAWSURFACE src, LPRECT srcRect, DWORD) override {
    if (!src) return DDERR_INVALIDPARAMS;
    auto* s = static_cast<Surface*>(src);
    RECT from = srcRect ? *srcRect : RECT{0, 0, s->Width(), s->Height()};
    RECT dst{static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(x) + from.right - from.left,
             static_cast<LONG>(y) + from.bottom - from.top};
    if (Clip(dst)) CopyScaled(*s, from, dst);
    if (primary_) Present();
    return DD_OK;
  }
  STDMETHODIMP Flip(LPDIRECTDRAWSURFACE, DWORD) override {
    if (primary_) Present();
    return DD_OK;
  }
  STDMETHODIMP GetSurfaceDesc(LPDDSURFACEDESC desc) override {
    if (!desc || desc->dwSize < sizeof(DDSURFACEDESC)) return DDERR_INVALIDPARAMS;
    Describe(*desc);
    return DD_OK;
  }
  STDMETHODIMP GetCaps(LPDDSCAPS caps) override {
    caps->dwCaps = primary_ ? (DDSCAPS_PRIMARYSURFACE | DDSCAPS_VISIBLE) : (DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY);
    return DD_OK;
  }
  STDMETHODIMP GetPixelFormat(LPDDPIXELFORMAT pf) override {
    FillPixelFormat(*pf);
    return DD_OK;
  }
  STDMETHODIMP IsLost() override { return DD_OK; }
  STDMETHODIMP Restore() override { return DD_OK; }
  STDMETHODIMP GetBltStatus(DWORD) override { return DD_OK; }
  STDMETHODIMP GetFlipStatus(DWORD) override { return DD_OK; }
  STDMETHODIMP GetAttachedSurface(LPDDSCAPS, LPDIRECTDRAWSURFACE*) override { return DDERR_NOTFOUND; }

  // Not used by StarCraft
  STDMETHODIMP AddAttachedSurface(LPDIRECTDRAWSURFACE) override { UNSUPPORTED("AddAttachedSurface"); }
  STDMETHODIMP AddOverlayDirtyRect(LPRECT) override { UNSUPPORTED("AddOverlayDirtyRect"); }
  STDMETHODIMP BltBatch(LPDDBLTBATCH, DWORD, DWORD) override { UNSUPPORTED("BltBatch"); }
  STDMETHODIMP DeleteAttachedSurface(DWORD, LPDIRECTDRAWSURFACE) override { UNSUPPORTED("DeleteAttachedSurface"); }
  STDMETHODIMP EnumAttachedSurfaces(LPVOID, LPDDENUMSURFACESCALLBACK) override { return DD_OK; }
  STDMETHODIMP EnumOverlayZOrders(DWORD, LPVOID, LPDDENUMSURFACESCALLBACK) override { UNSUPPORTED("EnumOverlayZOrders"); }
  STDMETHODIMP GetClipper(LPDIRECTDRAWCLIPPER*) override { return DDERR_NOCLIPPERATTACHED; }
  STDMETHODIMP GetColorKey(DWORD, LPDDCOLORKEY) override { return DDERR_NOCOLORKEY; }
  STDMETHODIMP GetDC(HDC*) override { UNSUPPORTED("Surface::GetDC"); }
  STDMETHODIMP GetOverlayPosition(LPLONG, LPLONG) override { UNSUPPORTED("GetOverlayPosition"); }
  STDMETHODIMP Initialize(LPDIRECTDRAW, LPDDSURFACEDESC) override { return DDERR_ALREADYINITIALIZED; }
  STDMETHODIMP ReleaseDC(HDC) override { UNSUPPORTED("Surface::ReleaseDC"); }
  STDMETHODIMP SetClipper(LPDIRECTDRAWCLIPPER) override { return DD_OK; }
  STDMETHODIMP SetColorKey(DWORD, LPDDCOLORKEY) override { UNSUPPORTED("SetColorKey"); }
  STDMETHODIMP SetOverlayPosition(LONG, LONG) override { UNSUPPORTED("SetOverlayPosition"); }
  STDMETHODIMP UpdateOverlay(LPRECT, LPDIRECTDRAWSURFACE, LPRECT, DWORD, LPDDOVERLAYFX) override { UNSUPPORTED("UpdateOverlay"); }
  STDMETHODIMP UpdateOverlayDisplay(DWORD) override { UNSUPPORTED("UpdateOverlayDisplay"); }
  STDMETHODIMP UpdateOverlayZOrder(DWORD, LPDIRECTDRAWSURFACE) override { UNSUPPORTED("UpdateOverlayZOrder"); }

 private:
  // With a primary redirect (widescreen), the primary surface is a view of the game's own screen buffer.
  bool Redirected() const { return primary_ && g_redirect.pixels && *g_redirect.pixels; }
  uint8_t* Base() { return Redirected() ? *g_redirect.pixels : pixels_.data(); }
  int Width() const { return Redirected() ? g_redirect.width : width_; }
  int Height() const { return Redirected() ? g_redirect.height : height_; }
  int Pitch() const { return Redirected() ? g_redirect.width : pitch_; }
  uint8_t* Row(LONG y) { return Base() + y * Pitch(); }

  void Describe(DDSURFACEDESC& desc) const {
    DWORD size = desc.dwSize;
    std::memset(&desc, 0, size);
    desc.dwSize = size;
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
    desc.dwWidth = Width();
    desc.dwHeight = Height();
    desc.lPitch = Pitch();
    desc.ddsCaps.dwCaps = primary_ ? DDSCAPS_PRIMARYSURFACE : (DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY);
    FillPixelFormat(desc.ddpfPixelFormat);
  }

  bool Clip(RECT& r) const {
    r.left = std::max<LONG>(r.left, 0);
    r.top = std::max<LONG>(r.top, 0);
    r.right = std::min<LONG>(r.right, Width());
    r.bottom = std::min<LONG>(r.bottom, Height());
    return r.left < r.right && r.top < r.bottom;
  }

  void CopyScaled(Surface& src, const RECT& from, const RECT& to) {
    LONG fw = from.right - from.left, fh = from.bottom - from.top;
    LONG tw = to.right - to.left, th = to.bottom - to.top;
    if (fw <= 0 || fh <= 0) return;
    for (LONG y = 0; y < th; ++y) {
      LONG sy = from.top + y * fh / th;
      if (sy < 0 || sy >= src.Height()) continue;
      const uint8_t* s = src.Row(sy);
      uint8_t* d = Row(to.top + y) + to.left;
      if (fw == tw && from.left >= 0 && from.right <= src.Width()) {
        std::memmove(d, s + from.left, tw);
        continue;
      }
      for (LONG x = 0; x < tw; ++x) {
        LONG sx = from.left + x * fw / tw;
        d[x] = (sx >= 0 && sx < src.Width()) ? s[sx] : 0;
      }
    }
  }

  void Present() {
    if (Redirected() && g_redirect.present) {
      g_redirect.present();
    } else {
      SubmitFrame(pixels_.data(), width_, height_, pitch_);
    }
  }

  ULONG refs_ = 1;
  int width_, height_, pitch_;
  bool primary_;
  std::vector<uint8_t> pixels_;
  Palette* palette_ = nullptr;
};

class DirectDraw final : public IDirectDraw {
 public:
  STDMETHODIMP QueryInterface(REFIID riid, LPVOID* out) override {
    if (riid == IID_IUnknown || riid == IID_IDirectDraw) {
      AddRef();
      *out = this;
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
  STDMETHODIMP_(ULONG) Release() override {
    ULONG n = --refs_;
    if (n == 0) {
      if (primary_) primary_->Release();
      delete this;
    }
    return n;
  }

  // Used by the game
  STDMETHODIMP SetCooperativeLevel(HWND window, DWORD flags) override {
    window_ = window;
    log::Info("ddraw: SetCooperativeLevel(hwnd=%p, flags=0x%X)", window, static_cast<unsigned>(flags));
    return DD_OK;
  }
  STDMETHODIMP SetDisplayMode(DWORD width, DWORD height, DWORD bpp) override {
    log::Info("ddraw: SetDisplayMode(%u, %u, %u)", static_cast<unsigned>(width), static_cast<unsigned>(height),
              static_cast<unsigned>(bpp));
    if (bpp != 8 || width == 0 || height == 0) return DDERR_INVALIDMODE;
    width_ = static_cast<int>(width);
    height_ = static_cast<int>(height);
    return DD_OK;
  }
  STDMETHODIMP RestoreDisplayMode() override { return DD_OK; }
  STDMETHODIMP CreatePalette(DWORD, LPPALETTEENTRY entries, LPDIRECTDRAWPALETTE* out, IUnknown*) override {
    *out = new Palette(entries);
    return DD_OK;
  }
  STDMETHODIMP CreateSurface(LPDDSURFACEDESC desc, LPDIRECTDRAWSURFACE* out, IUnknown*) override {
    if (!desc || !out) return DDERR_INVALIDPARAMS;
    bool primary = (desc->dwFlags & DDSD_CAPS) && (desc->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE);
    int w = width_, h = height_;
    if (!primary) {
      if (!(desc->dwFlags & DDSD_WIDTH) || !(desc->dwFlags & DDSD_HEIGHT)) return DDERR_INVALIDPARAMS;
      w = static_cast<int>(desc->dwWidth);
      h = static_cast<int>(desc->dwHeight);
    }
    auto* surface = new Surface(w, h, primary);
    log::Info("ddraw: CreateSurface(%s %dx%d)", primary ? "primary" : "offscreen", w, h);
    if (primary) {
      if (primary_) primary_->Release();
      primary_ = surface;
      primary_->AddRef();
      StartPresenter(window_, g_presenterOptions);
    }
    *out = surface;
    return DD_OK;
  }
  STDMETHODIMP GetGDISurface(LPDIRECTDRAWSURFACE* out) override {
    if (!primary_) return DDERR_NOTFOUND;
    primary_->AddRef();
    *out = primary_;
    return DD_OK;
  }
  STDMETHODIMP GetDisplayMode(LPDDSURFACEDESC desc) override {
    if (!desc || desc->dwSize < sizeof(DDSURFACEDESC)) return DDERR_INVALIDPARAMS;
    DWORD size = desc->dwSize;
    std::memset(desc, 0, size);
    desc->dwSize = size;
    desc->dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
    desc->dwWidth = width_;
    desc->dwHeight = height_;
    desc->lPitch = (width_ + 3) & ~3;
    desc->dwRefreshRate = 60;
    FillPixelFormat(desc->ddpfPixelFormat);
    return DD_OK;
  }
  STDMETHODIMP EnumDisplayModes(DWORD, LPDDSURFACEDESC, LPVOID context, LPDDENUMMODESCALLBACK callback) override {
    DDSURFACEDESC desc{};
    desc.dwSize = sizeof(desc);
    GetDisplayMode(&desc);
    if (callback) callback(&desc, context);
    return DD_OK;
  }
  STDMETHODIMP GetCaps(LPDDCAPS driverCaps, LPDDCAPS helCaps) override {
    for (LPDDCAPS caps : {driverCaps, helCaps}) {
      if (!caps) continue;
      DWORD size = caps->dwSize;
      std::memset(caps, 0, size);
      caps->dwSize = size;
      caps->dwCaps = DDCAPS_BLT | DDCAPS_PALETTE;
      caps->dwPalCaps = DDPCAPS_8BIT;
    }
    return DD_OK;
  }
  STDMETHODIMP GetVerticalBlankStatus(LPBOOL inVBlank) override {
    *inVBlank = TRUE;
    return DD_OK;
  }
  STDMETHODIMP WaitForVerticalBlank(DWORD, HANDLE) override { return DD_OK; }
  STDMETHODIMP GetMonitorFrequency(LPDWORD hz) override {
    *hz = 60;
    return DD_OK;
  }
  STDMETHODIMP FlipToGDISurface() override { return DD_OK; }
  STDMETHODIMP Initialize(GUID*) override { return DDERR_ALREADYINITIALIZED; }
  STDMETHODIMP Compact() override { return DD_OK; }

  // Not used by StarCraft
  STDMETHODIMP CreateClipper(DWORD, LPDIRECTDRAWCLIPPER*, IUnknown*) override { UNSUPPORTED("CreateClipper"); }
  STDMETHODIMP DuplicateSurface(LPDIRECTDRAWSURFACE, LPDIRECTDRAWSURFACE*) override { UNSUPPORTED("DuplicateSurface"); }
  STDMETHODIMP EnumSurfaces(DWORD, LPDDSURFACEDESC, LPVOID, LPDDENUMSURFACESCALLBACK) override { UNSUPPORTED("EnumSurfaces"); }
  STDMETHODIMP GetFourCCCodes(LPDWORD, LPDWORD) override { UNSUPPORTED("GetFourCCCodes"); }
  STDMETHODIMP GetScanLine(LPDWORD) override { UNSUPPORTED("GetScanLine"); }

 private:
  ULONG refs_ = 1;
  HWND window_ = nullptr;
  int width_ = 640, height_ = 480;
  Surface* primary_ = nullptr;
};

}  // namespace

void SetPresenterOptions(const PresenterOptions& options) { g_presenterOptions = options; }
void SetPrimaryRedirect(const PrimaryRedirect& redirect) { g_redirect = redirect; }

HRESULT WINAPI EmulatedDirectDrawCreate(GUID*, LPDIRECTDRAW* out, IUnknown*) {
  if (!out) return DDERR_INVALIDPARAMS;
  *out = new DirectDraw();
  log::Info("ddraw: DirectDrawCreate -> emulated device");
  return DD_OK;
}

}  // namespace scpatch::render
