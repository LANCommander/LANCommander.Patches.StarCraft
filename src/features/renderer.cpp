// Replaces the game's exclusive-fullscreen 640x480x8 DirectDraw output with an emulated DirectDraw whose frames
// are presented through Direct3D 11 (borderless or windowed, scaled to the window, vsynced).
//
// The game assumes its window is a 640x480 fullscreen surface at the screen origin: it reads GetCursorPos as
// game coordinates (edge scrolling tests x < 2), warps the cursor with raw SetCursorPos(320, 200), and clips
// the cursor to ClientToScreen(0,0)-(640,480). So for the main window, mouse messages and the cursor APIs are
// translated between that game space and the real, scaled viewport. Window geometry APIs are left alone: the
// game only uses them to centre dialogs. Battle.net's GDI dialog windows are drawn into the presented frame and
// get their mouse input through the game thread's message hook (see "Storm dialog windows" below).

#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/patch.h"
#include "core/paths.h"
#include "features/features.h"
#include "render/ddraw_emu.h"
#include "render/presenter.h"

namespace scpatch::features {
namespace {

using render::ClientToGame;
using render::GameToClient;

constexpr char kGameWindowClass[] = "SWarClass";

HWND g_gameWindow;
WNDPROC g_gameWndProc;
bool g_windowed;
int g_windowScale = 2;

bool IsGameWindow(HWND window) { return window && window == g_gameWindow; }
void InstallDialogSupport();

// Game space <-> real screen coordinates, via the window's client area.
POINT GameToScreen(POINT game) {
  POINT p = GameToClient(game);
  ClientToScreen(g_gameWindow, &p);
  return p;
}

POINT ScreenToGame(POINT screen) {
  ScreenToClient(g_gameWindow, &screen);
  return ClientToGame(screen);
}

LRESULT CALLBACK GameWndProc(HWND window, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (window == g_gameWindow) {
    if (msg == WM_KEYDOWN && wParam == VK_F12) {
      // Full-size capture of what the presenter is showing (the game's own screenshot key is 640 wide).
      SYSTEMTIME t;
      GetLocalTime(&t);
      wchar_t path[MAX_PATH];
      swprintf_s(path, L"%ls\\Screenshots\\scpatch_%04u%02u%02u_%02u%02u%02u.bmp", paths::UserDataRoot().c_str(),
                 t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
      paths::EnsureDirectory(paths::UserDataRoot() + L"\\Screenshots");
      bool ok = render::SaveLastFrame(path);
      log::Info("renderer: F12 capture %s: %ls", ok ? "saved" : "FAILED", path);
      std::wstring state = path;
      state.replace(state.size() - 4, 4, L".state");
      DumpWidescreenState(state.c_str());
      DumpZoomBuffers(std::wstring(path, wcslen(path) - 4));
    }
    if (msg == WM_KEYDOWN && !(lParam & (1 << 30))) SelectionKey(wParam);  // not on auto-repeat
    if (msg == WM_MOUSEWHEEL && ZoomWheel(GET_WHEEL_DELTA_WPARAM(wParam))) return 0;
    if ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST && msg != WM_MOUSEWHEEL && msg != WM_MOUSEHWHEEL)) {
      POINT g = ClientToGame(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
      lParam = MAKELPARAM(g.x, g.y);
    } else if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) {
      POINT g = ScreenToGame(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
      lParam = MAKELPARAM(g.x, g.y);
    }
  }
  return CallWindowProcA(g_gameWndProc, window, msg, wParam, lParam);
}

FARPROC WINAPI Hook_GetProcAddress(HMODULE module, LPCSTR name) {
  if (HIWORD(reinterpret_cast<uintptr_t>(name)) && std::strcmp(name, "DirectDrawCreate") == 0) {
    return reinterpret_cast<FARPROC>(&render::EmulatedDirectDrawCreate);
  }
  return GetProcAddress(module, name);
}

ATOM WINAPI Hook_RegisterClassExA(const WNDCLASSEXA* wc) {
  if (wc && HIWORD(reinterpret_cast<uintptr_t>(wc->lpszClassName)) &&
      _stricmp(wc->lpszClassName, kGameWindowClass) == 0) {
    WNDCLASSEXA copy = *wc;
    g_gameWndProc = copy.lpfnWndProc;
    copy.lpfnWndProc = GameWndProc;
    return RegisterClassExA(&copy);
  }
  return RegisterClassExA(wc);
}

HWND WINAPI Hook_CreateWindowExA(DWORD exStyle, LPCSTR className, LPCSTR title, DWORD style, int x, int y, int w,
                                 int h, HWND parent, HMENU menu, HINSTANCE instance, LPVOID param) {
  bool game = HIWORD(reinterpret_cast<uintptr_t>(className)) && _stricmp(className, kGameWindowClass) == 0;
  if (game && g_windowed) {
    style = (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW;
    RECT r{0, 0, 640 * g_windowScale, 480 * g_windowScale};
    AdjustWindowRectEx(&r, style, FALSE, exStyle);
    w = r.right - r.left;
    h = r.bottom - r.top;
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    x = work.left + (work.right - work.left - w) / 2;
    y = work.top + (work.bottom - work.top - h) / 2;
  }
  HWND window = CreateWindowExA(exStyle, className, title, style, x, y, w, h, parent, menu, instance, param);
  if (game) {
    g_gameWindow = window;
    log::Info("renderer: game window %p created (%dx%d, %s)", window, w, h, g_windowed ? "windowed" : "borderless");
    if (window) InstallDialogSupport();
  }
  return window;
}

BOOL WINAPI Hook_GetCursorPos(LPPOINT p) {
  if (!GetCursorPos(p)) return FALSE;
  if (g_gameWindow) *p = ScreenToGame(*p);
  return TRUE;
}

// Cursor warps and clips are rare, so the first few are logged to make mapping problems diagnosable.
int g_cursorLogBudget = 20;

BOOL WINAPI Hook_SetCursorPos(int x, int y) {
  if (!g_gameWindow) return SetCursorPos(x, y);
  POINT s = GameToScreen(POINT{x, y});
  BOOL ok = SetCursorPos(s.x, s.y);
  if (g_cursorLogBudget > 0 && g_cursorLogBudget--) {
    log::Info("renderer: SetCursorPos game(%d,%d) -> screen(%ld,%ld) ok=%d", x, y, s.x, s.y, ok);
  }
  return ok;
}

// A clip the game asks for before its window and the presenter exist (it clips while handling its first
// activation, inside CreateWindowExA) cannot be mapped yet. Passed through raw it confined the real cursor to the
// monitor's top-left 896 x 480 pixels until the game happened to clip again, which the Battle.net screens never
// do. It is kept and applied, mapped, with the first frame.
bool g_pendingClip;
RECT g_pendingClipRect;

// The clip as the system reported it right after we applied it. On a fresh start something outside the hooked
// modules (Windows' DWM8And16BitMitigation compatibility layer, which it applies to StarCraft by itself, is the
// suspect) re-clips the cursor to the raw top-left 640 x 480 after our mapped clip, until an alt-tab makes the
// game clip again. So the clip is checked every frame and restored while the game window is in the foreground.
bool g_clipActive;
RECT g_appliedClip;

BOOL WINAPI Hook_ClipCursor(const RECT* rect) {
  if (!rect) {
    g_pendingClip = false;
    g_clipActive = false;
    return ClipCursor(nullptr);
  }
  if (!g_gameWindow || !render::FrameSubmitted()) {
    g_pendingClip = true;
    g_pendingClipRect = *rect;
    return TRUE;
  }
  g_pendingClip = false;
  // The game passes an exclusive game-space rectangle; clip to the centres of its edge pixels. With widescreen
  // the game's rectangle is the full buffer width even in menus, where only 640 columns are shown.
  SIZE frame = render::GameSize();
  POINT tl = GameToScreen(POINT{rect->left, rect->top});
  POINT br = GameToScreen(POINT{std::min<LONG>(rect->right, frame.cx) - 1, std::min<LONG>(rect->bottom, frame.cy) - 1});
  RECT r{tl.x, tl.y, br.x + 1, br.y + 1};
  if (g_cursorLogBudget > 0 && g_cursorLogBudget--) {
    log::Info("renderer: ClipCursor game(%ld,%ld,%ld,%ld) -> screen(%ld,%ld,%ld,%ld)", rect->left, rect->top,
              rect->right, rect->bottom, r.left, r.top, r.right, r.bottom);
  }
  BOOL ok = ClipCursor(&r);
  g_clipActive = ok && GetClipCursor(&g_appliedClip);
  return ok;
}

int g_clipRestoreLogBudget = 5;

void RestoreClip() {
  RECT current;
  if (!g_clipActive || GetForegroundWindow() != g_gameWindow || !GetClipCursor(&current) ||
      EqualRect(&current, &g_appliedClip)) {
    return;
  }
  if (g_clipRestoreLogBudget > 0 && g_clipRestoreLogBudget--) {
    log::Info("renderer: cursor clip changed outside the game to (%ld,%ld,%ld,%ld); restoring (%ld,%ld,%ld,%ld)",
              current.left, current.top, current.right, current.bottom, g_appliedClip.left, g_appliedClip.top,
              g_appliedClip.right, g_appliedClip.bottom);
  }
  ClipCursor(&g_appliedClip);
}

// In game space the window's client area sits at the screen origin.
BOOL WINAPI Hook_ClientToScreen(HWND window, LPPOINT p) {
  return IsGameWindow(window) ? TRUE : ClientToScreen(window, p);
}

BOOL WINAPI Hook_ScreenToClient(HWND window, LPPOINT p) {
  return IsGameWindow(window) ? TRUE : ScreenToClient(window, p);
}

// ---- Storm dialog windows (Battle.net) -------------------------------------------------------------------
// Battle.net's screens are real Win32 windows (Storm's SDlgDialog popups with SDlgStatic text, buttons, edit
// boxes and lists), placed in game space at the screen origin. Their artwork goes through the DirectDraw
// surface; only text and controls are drawn by GDI, onto windows that leave their background unpainted, so in
// exclusive fullscreen the surface showed through. Under the scaled presenter those windows would sit unscaled
// in the monitor's top-left corner. Instead they are cloaked (they keep their positions, which Storm uses to
// place the artwork), captured every frame into an overlay (black = unpainted = transparent) that the presenter
// scales with the frame, and the game thread's mouse messages are re-addressed to the dialog control under the
// mapped game point.

constexpr char kDialogClass[] = "SDlgDialog";

bool g_dialogsVisible;  // a dialog was captured with the last frame
HHOOK g_messageHook;
std::vector<uint32_t> g_overlayPixels;
bool g_overlayShown;

bool IsDialogRoot(HWND window) {
  char name[16];
  return GetClassNameA(window, name, sizeof(name)) && std::strcmp(name, kDialogClass) == 0;
}

bool IsDialogTree(HWND window) { return window && IsDialogRoot(GetAncestor(window, GA_ROOT)); }

// Visible top-level dialogs of this process, topmost first.
std::vector<HWND> VisibleDialogs() {
  std::vector<HWND> out;
  EnumWindows(
      [](HWND window, LPARAM param) -> BOOL {
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid == GetCurrentProcessId() && IsWindowVisible(window) && IsDialogRoot(window)) {
          reinterpret_cast<std::vector<HWND>*>(param)->push_back(window);
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&out));
  return out;
}

// Cloaked (DWMWA_CLOAK): DWM still composes the window, so PrintWindow can read it, but does not show it. Not
// layered: Storm clips each dialog's artwork to the window's visible region, and a layered window's region no
// longer matches its screen place (the artwork vanished from the frame). A cloaked window can still be under the
// real cursor in the monitor's top-left corner; the message hook re-addresses those events like all others.
constexpr char kCloakedProp[] = "scpatch.cloaked";

void HideDialog(HWND window) {
  using SetAttribute = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
  static auto setAttribute =
      reinterpret_cast<SetAttribute>(GetProcAddress(LoadLibraryW(L"dwmapi.dll"), "DwmSetWindowAttribute"));
  if (GetPropA(window, kCloakedProp)) return;
  SetPropA(window, kCloakedProp, reinterpret_cast<HANDLE>(1));
  BOOL cloak = TRUE;
  HRESULT hr = setAttribute ? setAttribute(window, 13 /* DWMWA_CLOAK */, &cloak, sizeof(cloak)) : E_NOTIMPL;
  if (FAILED(hr)) log::Warn("renderer: could not cloak dialog %p (0x%08X)", window, static_cast<unsigned>(hr));
  DWORD thread = GetWindowThreadProcessId(window, nullptr);
  if (thread != GetWindowThreadProcessId(g_gameWindow, nullptr)) {
    log::Warn("renderer: dialog %p belongs to thread %u, not the game window's: its mouse input is not mapped",
              window, static_cast<unsigned>(thread));
  }
}

// Overlay provider (game thread, before each frame is handed to the presenter).
void CaptureDialogs(int width, int height) {
  if (g_pendingClip && render::FrameSubmitted()) {
    RECT r = g_pendingClipRect;
    Hook_ClipCursor(&r);
  }
  RestoreClip();
  std::vector<HWND> dialogs = VisibleDialogs();
  g_dialogsVisible = !dialogs.empty();
  if (dialogs.empty()) {
    if (g_overlayShown) render::SubmitOverlay(nullptr, 0, 0);
    g_overlayShown = false;
    return;
  }
  g_overlayPixels.assign(static_cast<size_t>(width) * height, 0);
  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  for (auto it = dialogs.rbegin(); it != dialogs.rend(); ++it) {  // bottom to top
    HWND window = *it;
    HideDialog(window);
    RECT r;
    if (!GetWindowRect(window, &r)) continue;  // game space: Storm places its dialogs there
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) continue;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) continue;
    HGDIOBJ old = SelectObject(mem, dib);
    // PW_RENDERFULLCONTENT reads the window's composed content, which a cloaked window still has.
    if (PrintWindow(window, mem, PW_RENDERFULLCONTENT)) {
      GdiFlush();
      const uint32_t* src = static_cast<const uint32_t*>(bits);
      for (int y = 0; y < h; ++y) {
        int gy = r.top + y;
        if (gy < 0 || gy >= height) continue;
        for (int x = 0; x < w; ++x) {
          int gx = r.left + x;
          if (gx < 0 || gx >= width) continue;
          uint32_t c = src[static_cast<size_t>(y) * w + x] & 0x00FFFFFF;
          if (c) g_overlayPixels[static_cast<size_t>(gy) * width + gx] = 0xFF000000u | c;
        }
      }
    }
    SelectObject(mem, old);
    DeleteObject(dib);
  }
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  render::SubmitOverlay(g_overlayPixels.data(), width, height);
  g_overlayShown = true;
}

// The deepest enabled child of `parent` under game point `g` that does not pass the mouse through (statics
// answer HTTRANSPARENT), the way the system's own hit testing descends.
HWND DialogChildAt(HWND parent, POINT g) {
  for (HWND child = GetWindow(parent, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
    RECT r;
    if (!IsWindowVisible(child) || !IsWindowEnabled(child) || !GetWindowRect(child, &r) || !PtInRect(&r, g)) continue;
    if (SendMessageA(child, WM_NCHITTEST, 0, MAKELPARAM(g.x, g.y)) == HTTRANSPARENT) continue;
    return DialogChildAt(child, g);
  }
  return parent;
}

// The dialog window that gets a mouse event at game point `g`: nullptr if no dialog is there (the game window
// keeps it), INVALID_HANDLE_VALUE if a disabled dialog is (a modal box is open over it: drop the event).
HWND DialogAt(POINT g) {
  for (HWND dialog : VisibleDialogs()) {
    RECT r;
    if (!GetWindowRect(dialog, &r) || !PtInRect(&r, g)) continue;
    if (!IsWindowEnabled(dialog)) return static_cast<HWND>(INVALID_HANDLE_VALUE);
    return DialogChildAt(dialog, g);
  }
  return nullptr;
}

LRESULT CALLBACK DialogMessageHook(int code, WPARAM wParam, LPARAM lParam) {
  MSG* msg = reinterpret_cast<MSG*>(lParam);
  // PM_REMOVE only: a peeked message comes through again when it is removed and must be mapped once.
  if (code == HC_ACTION && wParam == PM_REMOVE && g_dialogsVisible && msg->message >= WM_MOUSEFIRST &&
      msg->message <= WM_MOUSELAST && msg->message != WM_MOUSEWHEEL && msg->message != WM_MOUSEHWHEEL) {
    POINT g = ScreenToGame(msg->pt);  // msg->pt is the real cursor position
    HWND capture = GetCapture();
    HWND target = IsDialogTree(capture) ? capture : DialogAt(g);
    if (target == static_cast<HWND>(INVALID_HANDLE_VALUE)) {
      msg->message = WM_NULL;
    } else if (target) {
      POINT c = g;  // dialogs sit in game space, so the real ScreenToClient gives client coordinates
      ScreenToClient(target, &c);
      msg->hwnd = target;
      msg->lParam = MAKELPARAM(c.x, c.y);
      msg->pt = g;
    }
  }
  return CallNextHookEx(g_messageHook, code, wParam, lParam);
}

void InstallDialogSupport() {
  render::SetOverlayProvider(&CaptureDialogs);
  g_messageHook = SetWindowsHookExW(WH_GETMESSAGE, &DialogMessageHook, nullptr, GetCurrentThreadId());
  if (!g_messageHook) {
    log::Warn("renderer: no message hook (error %u); Battle.net dialogs will not take mouse input",
              static_cast<unsigned>(GetLastError()));
  }
}

void HookCursorApis(HMODULE module, const char* label);

// Storm loads the network providers (battle.snp, standard.snp) with LoadLibraryA. Battle.net's dialog code in
// battle.snp reads GetCursorPos itself (a button checks the cursor is still over it when released), so the
// providers get the same cursor hooks as the game and Storm. Storm frees providers after enumerating them and
// on leaving multiplayer, and a reload (usually at the same base) gets a fresh IAT, so this runs on every load.
// Rewriting an entry that already points at our hook is harmless.
void HookProvider(HMODULE module, const char* name) {
  if (!module) return;
  HookCursorApis(module, name);
}

bool IsProviderName(const char* name) {
  size_t n = name ? std::strlen(name) : 0;
  return n > 4 && _stricmp(name + n - 4, ".snp") == 0;
}

HMODULE WINAPI Hook_LoadLibraryA(LPCSTR name) {
  HMODULE module = LoadLibraryA(name);
  if (module && IsProviderName(name)) HookProvider(module, name);
  return module;
}

void HookLoadedProviders() {
  for (const char* name : {"battle.snp", "standard.snp"}) HookProvider(GetModuleHandleA(name), name);
}

render::Scaling ParseScaling(const std::wstring& s) {
  if (_wcsicmp(s.c_str(), L"integer") == 0) return render::Scaling::Integer;
  if (_wcsicmp(s.c_str(), L"stretch") == 0) return render::Scaling::Stretch;
  return render::Scaling::Aspect;
}

void HookCursorApis(HMODULE module, const char* label) {
  const struct {
    const char* name;
    void* hook;
  } hooks[] = {
      {"GetCursorPos", reinterpret_cast<void*>(&Hook_GetCursorPos)},
      {"SetCursorPos", reinterpret_cast<void*>(&Hook_SetCursorPos)},
      {"ClipCursor", reinterpret_cast<void*>(&Hook_ClipCursor)},
      {"ClientToScreen", reinterpret_cast<void*>(&Hook_ClientToScreen)},
      {"ScreenToClient", reinterpret_cast<void*>(&Hook_ScreenToClient)},
  };
  int hooked = 0;
  for (const auto& h : hooks) {
    if (patch::HookImport(module, "USER32.dll", h.name, h.hook, nullptr)) ++hooked;
  }
  log::Info("renderer: hooked %d cursor API(s) in %s", hooked, label);
}

}  // namespace

void InstallRenderer() {
  if (!config::GetBool(L"Renderer", L"Enabled", true)) {
    log::Info("renderer: disabled in config; using the game's DirectDraw output");
    return;
  }

  render::PresenterOptions options;
  options.scaling = ParseScaling(config::GetString(L"Renderer", L"Scaling", L"aspect"));
  options.filter = _wcsicmp(config::GetString(L"Renderer", L"Filter", L"sharp").c_str(), L"nearest") == 0
                       ? render::Filter::Nearest
                       : render::Filter::SharpBilinear;
  options.vsync = config::GetBool(L"Renderer", L"VSync", true);
  render::SetPresenterOptions(options);
  g_windowed = config::GetBool(L"Renderer", L"Windowed", false);
  g_windowScale = config::GetInt(L"Renderer", L"WindowScale", 2);
  if (g_windowScale < 1) g_windowScale = 1;

  // StarCraft is DPI-unaware, so with display scaling Windows would hand it virtualized coordinates and then
  // upscale our output again. Opting in to per-monitor DPI awareness (before any window exists) lets us render
  // at the panel's native resolution and keeps mouse coordinates in real pixels.
  if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
    log::Warn("renderer: could not enable per-monitor DPI awareness (error %u)", static_cast<unsigned>(GetLastError()));
  }

  HMODULE exe = GetModuleHandleW(nullptr);
  bool ok = patch::HookImport(exe, "KERNEL32.dll", "GetProcAddress", reinterpret_cast<void*>(&Hook_GetProcAddress),
                              nullptr) &&
            patch::HookImport(exe, "USER32.dll", "RegisterClassExA",
                              reinterpret_cast<void*>(&Hook_RegisterClassExA), nullptr) &&
            patch::HookImport(exe, "USER32.dll", "CreateWindowExA", reinterpret_cast<void*>(&Hook_CreateWindowExA),
                              nullptr);
  if (!ok) {
    log::Error("renderer: could not hook window/DirectDraw creation; renderer not installed");
    return;
  }
  HookCursorApis(exe, "StarCraft.exe");
  if (HMODULE storm = GetModuleHandleW(L"storm.dll")) {
    HookCursorApis(storm, "storm.dll");
    if (!patch::HookImport(storm, "KERNEL32.dll", "LoadLibraryA", reinterpret_cast<void*>(&Hook_LoadLibraryA), nullptr)) {
      log::Warn("renderer: could not hook storm.dll's LoadLibraryA; Battle.net may not see the mapped cursor");
    }
  }
  HookLoadedProviders();
  log::Info("renderer: installed (%s, scaling=%ls, vsync=%d)", g_windowed ? "windowed" : "borderless",
            config::GetString(L"Renderer", L"Scaling", L"aspect").c_str(), options.vsync);
}

}  // namespace scpatch::features
