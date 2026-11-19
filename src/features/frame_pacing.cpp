// Smooth, high-refresh-rate presentation without changing game speed (see re/notes/timing.md).
//
// 1. High-resolution time: the game paces simulation frames with GetTickCount, whose ~15.6 ms resolution turns
//    the 42 ms "Fastest" frame into an uneven 31/47 ms cadence. GetTickCount is replaced (in StarCraft.exe and
//    storm.dll) by a QPC-based millisecond clock, and the system timer is raised to 1 ms so the optional CPU
//    throttle's Sleep(1) really sleeps ~1 ms. Lockstep networking depends on turns, not wall time, so this
//    cannot desync anything.
//
// 2. Smooth scrolling: the in-game loop (FUN_004d9840) runs the scroll logic (FUN_0047f0e0) and requests a
//    redraw on a "more than 10 ms passed" GetTickCount gate, i.e. ~64 Hz in practice, moving the view in steps
//    of up to 64 px. The gate at 0x4D98C2 is replaced by ScrollGate, which
//      - runs the unmodified scroll logic on a fixed 64 Hz clock, so scroll speed and acceleration match the
//        original game exactly;
//      - captures the four step calls it makes and spreads each step evenly over that 15.6 ms interval,
//        redrawing at the display's refresh rate;
//      - applies whatever is left before the next scroll tick, so the logic always sees the original position
//        (scripted camera pans compute from it).
//    ScrollGranularity keeps intermediate positions on a pixel grid (8 matches the game's own step sizes).

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "core/config.h"
#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

namespace scpatch::features {
namespace {

// StarCraft.exe 1.16.1 addresses (verified by disassembly).
constexpr uintptr_t kScrollGateSite = 0x004D98C2;  // start of the 10 ms gate in the in-game loop
constexpr uintptr_t kScrollLogic = 0x0047F0E0;     // edge/keyboard scroll logic, once per gate tick
constexpr uintptr_t kMarkMapDirty = 0x004BD350;    // marks the map layer for redraw
constexpr uintptr_t kStepUp = 0x0049C360;          // step in EAX, returns nonzero if the view moved
constexpr uintptr_t kStepDown = 0x0049C280;        // step in EDX
constexpr uintptr_t kStepLeft = 0x0049C1A0;        // step in EAX
constexpr uintptr_t kStepRight = 0x0049C0C0;       // step in EDX
constexpr uintptr_t kCallStepUp = 0x0047F210;      // the four step calls inside the scroll logic
constexpr uintptr_t kCallStepDown = 0x0047F226;
constexpr uintptr_t kCallStepLeft = 0x0047F247;
constexpr uintptr_t kCallStepRight = 0x0047F25B;

constexpr double kScrollTickSeconds = 1.0 / 64.0;  // the cadence the original gate runs at on Windows

LARGE_INTEGER g_qpcFrequency;
LARGE_INTEGER g_qpcBase;
DWORD g_tickBase;

double g_frameSeconds;  // minimum time between smooth-scroll redraws
int g_granularity = 8;

// Scroll motion captured during the current tick: signed pixels per axis, and how much has been applied.
struct Axis {
  int total;
  int applied;
};
Axis g_x, g_y;
int64_t g_tickStart, g_nextTick, g_nextFrame;
bool g_clockStarted;

int64_t Now() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
}

int64_t Seconds(double s) { return static_cast<int64_t>(s * g_qpcFrequency.QuadPart); }

DWORD WINAPI Hook_GetTickCount() {
  return g_tickBase + static_cast<DWORD>((Now() - g_qpcBase.QuadPart) * 1000 / g_qpcFrequency.QuadPart);
}

// Calls one of the game's step functions, which take the step in EAX or EDX.
int CallStepEax(uintptr_t fn, int amount) {
  int moved;
  __asm {
    mov eax, amount
    call dword ptr [fn]
    mov moved, eax
  }
  return moved;
}

int CallStepEdx(uintptr_t fn, int amount) {
  int moved;
  __asm {
    mov edx, amount
    call dword ptr [fn]
    mov moved, eax
  }
  return moved;
}

// Moves the view by `delta` pixels along one axis using the game's own (clamping) step functions.
bool Move(Axis& axis, bool horizontal, int delta) {
  if (delta == 0) return false;
  axis.applied += delta;
  if (horizontal) return delta > 0 ? CallStepEdx(kStepRight, delta) : CallStepEax(kStepLeft, -delta);
  return delta > 0 ? CallStepEdx(kStepDown, delta) : CallStepEax(kStepUp, -delta);
}

// Brings `axis` to `fraction` of its total, rounded towards zero onto the granularity grid.
bool Advance(Axis& axis, bool horizontal, double fraction) {
  int target = static_cast<int>(axis.total * fraction);
  if (fraction < 1.0) target = target / g_granularity * g_granularity;
  return Move(axis, horizontal, target - axis.applied);
}

// Records a step the scroll logic wanted to make instead of moving immediately.
int __cdecl RecordStep(int axisIndex, int signedAmount) {
  Axis& axis = axisIndex == 0 ? g_x : g_y;
  axis.total = signedAmount;
  axis.applied = 0;
  return signedAmount != 0;
}

#define DEFINE_STEP_CAPTURE(name, reg, axisIndex, sign) \
  __declspec(naked) void name() {                       \
    __asm { mov ecx, reg }                              \
    __asm { imul ecx, ecx, sign }                       \
    __asm { push ecx }                                  \
    __asm { push axisIndex }                            \
    __asm { call RecordStep }                           \
    __asm { add esp, 8 }                                \
    __asm { ret }                                       \
  }
DEFINE_STEP_CAPTURE(CaptureStepUp, eax, 1, -1)
DEFINE_STEP_CAPTURE(CaptureStepDown, edx, 1, 1)
DEFINE_STEP_CAPTURE(CaptureStepLeft, eax, 0, -1)
DEFINE_STEP_CAPTURE(CaptureStepRight, edx, 0, 1)
#undef DEFINE_STEP_CAPTURE

double DisplayRefreshRate() {
  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1) {
    return mode.dmDisplayFrequency;
  }
  return 60.0;
}

// Replaces the original gate. Returns true when the screen should be redrawn this loop iteration.
bool __cdecl ScrollGate() {
  int64_t now = Now();
  if (!g_clockStarted) {
    g_clockStarted = true;
    g_nextTick = g_nextFrame = now;
  }

  bool redraw = false;
  if (now >= g_nextTick) {
    // Finish the previous tick's motion so the logic sees exactly the position the original game would have.
    // After a long gap (the loop was not running, e.g. between games) the leftover belongs to an old view.
    bool stale = now - g_nextTick > Seconds(kScrollTickSeconds * 4);
    if (!stale && (Advance(g_x, true, 1.0) | Advance(g_y, false, 1.0))) {
      reinterpret_cast<void (*)()>(kMarkMapDirty)();
    }
    g_x = g_y = Axis{};

    reinterpret_cast<void (*)()>(kScrollLogic)();
    redraw = true;  // the original gate always requested a redraw after running the scroll logic

    g_tickStart = now;
    g_nextTick += Seconds(kScrollTickSeconds);
    if (now - g_nextTick > Seconds(kScrollTickSeconds * 4)) g_nextTick = now + Seconds(kScrollTickSeconds);
  }

  if ((g_x.total != g_x.applied || g_y.total != g_y.applied) && now >= g_nextFrame) {
    double fraction = std::clamp(static_cast<double>(now - g_tickStart) / Seconds(kScrollTickSeconds), 0.0, 1.0);
    bool moved = Advance(g_x, true, fraction) | Advance(g_y, false, fraction);
    if (moved) {
      reinterpret_cast<void (*)()>(kMarkMapDirty)();
      redraw = true;
    }
    g_nextFrame = now + Seconds(g_frameSeconds);
  }
  return redraw;
}

bool InstallSmoothScrolling() {
  // 0x4D98C2..0x4D98E7 (37 bytes): the GetTickCount-based 10 ms gate, the scroll logic call, and setting the
  // loop's redraw flag ([EBP-5]). Replaced with: call ScrollGate; if (al) redraw = 1.
  uintptr_t gate = reinterpret_cast<uintptr_t>(&ScrollGate);
  int32_t rel = static_cast<int32_t>(gate - (kScrollGateSite + 5));
  uint8_t r[4];
  std::memcpy(r, &rel, sizeof(rel));
  bool ok = patch::Bytes(
      kScrollGateSite,
      {0x8B, 0x0D, 0x8C, 0xCE, 0x51, 0x00,  // mov ecx, [0x51CE8C]
       0x03, 0x4D, 0xF4,                    // add ecx, [ebp-0xC]
       0x83, 0xF9, 0x0A,                    // cmp ecx, 10
       0x76, 0x17,                          // jbe 0x4D98E7
       0x8B, 0x55, 0xF4,                    // mov edx, [ebp-0xC]
       0xF7, 0xD2,                          // not edx
       0x83, 0xC2, 0x01,                    // add edx, 1
       0x89, 0x15, 0x8C, 0xCE, 0x51, 0x00,  // mov [0x51CE8C], edx
       0xE8, 0xFD, 0x57, 0xFA, 0xFF,        // call 0x47F0E0
       0xC6, 0x45, 0xFB, 0x01},             // mov byte [ebp-5], 1
      {0xE8, r[0], r[1], r[2], r[3],        // call ScrollGate
       0x84, 0xC0,                          // test al, al
       0x74, 0x1C,                          // jz 0x4D98E7
       0xC6, 0x45, 0xFB, 0x01,              // mov byte [ebp-5], 1
       0xEB, 0x16,                          // jmp 0x4D98E7
       0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
       0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90},
      "scroll gate");
  if (!ok) return false;

  ok = patch::RedirectCall(kCallStepUp, kStepUp, reinterpret_cast<void*>(&CaptureStepUp), "scroll step up") &&
       patch::RedirectCall(kCallStepDown, kStepDown, reinterpret_cast<void*>(&CaptureStepDown), "scroll step down") &&
       patch::RedirectCall(kCallStepLeft, kStepLeft, reinterpret_cast<void*>(&CaptureStepLeft), "scroll step left") &&
       patch::RedirectCall(kCallStepRight, kStepRight, reinterpret_cast<void*>(&CaptureStepRight),
                           "scroll step right");
  return ok;
}

}  // namespace

void InstallFramePacing() {
  if (!config::GetBool(L"FramePacing", L"Enabled", true)) {
    log::Info("frame pacing: disabled in config");
    return;
  }
  QueryPerformanceFrequency(&g_qpcFrequency);
  QueryPerformanceCounter(&g_qpcBase);
  g_tickBase = GetTickCount();

  if (config::GetBool(L"FramePacing", L"HighResolutionTimer", true)) {
    // timeBeginPeriod lives in winmm; load it lazily rather than adding a static import to the proxy.
    if (HMODULE winmm = LoadLibraryW(L"winmm.dll")) {
      if (auto begin = reinterpret_cast<UINT(WINAPI*)(UINT)>(GetProcAddress(winmm, "timeBeginPeriod"))) begin(1);
    }
    int hooked = 0;
    hooked += patch::HookImport(GetModuleHandleW(nullptr), "KERNEL32.dll", "GetTickCount",
                                reinterpret_cast<void*>(&Hook_GetTickCount), nullptr);
    if (HMODULE storm = GetModuleHandleW(L"storm.dll")) {
      hooked += patch::HookImport(storm, "KERNEL32.dll", "GetTickCount", reinterpret_cast<void*>(&Hook_GetTickCount),
                                  nullptr);
    }
    log::Info("frame pacing: high-resolution GetTickCount in %d module(s), 1 ms timer", hooked);
  }

  if (config::GetBool(L"FramePacing", L"SmoothScrolling", true)) {
    int maxFps = config::GetInt(L"FramePacing", L"MaxFps", 0);
    double fps = maxFps > 0 ? maxFps : DisplayRefreshRate();
    g_frameSeconds = 1.0 / fps;
    g_granularity = std::clamp(config::GetInt(L"FramePacing", L"ScrollGranularity", 8), 1, 32);
    if (InstallSmoothScrolling()) {
      log::Info("frame pacing: smooth scrolling at up to %.0f fps, %d px granularity", fps, g_granularity);
    } else {
      log::Error("frame pacing: smooth scrolling not installed");
    }
  }
}

}  // namespace scpatch::features
