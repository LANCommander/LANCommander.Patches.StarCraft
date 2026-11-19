#include "core/main.h"

#include <LANCommander.Interposer.Embedded.h>

#include <cstring>

#include "core/config.h"
#include "core/crash.h"
#include "core/log.h"
#include "core/patch.h"
#include "core/paths.h"
#include "features/features.h"

namespace scpatch {
namespace {

// StarCraft.exe 1.16.1 (Brood War). Every address in this project is specific to this build.
constexpr DWORD kExpectedTimeDateStamp = 0x496589CA;
constexpr DWORD kExpectedSizeOfImage = 0x002EC000;

uintptr_t g_entry;
uint8_t g_entryBytes[5];

bool g_interposerAttached;
bool g_interposerOwner;  // this copy installed the Interposer's hooks (false: another Interposer was loaded first)

void LogInterposer() {
  static const struct {
    DWORD flag;
    const char* name;
  } kFeatures[] = {
      {INTERPOSER_FEATURE_FLAG_REGISTRY, "Registry"},       {INTERPOSER_FEATURE_FLAG_FILES, "Files"},
      {INTERPOSER_FEATURE_FLAG_FASTDL, "FastDL"},           {INTERPOSER_FEATURE_FLAG_DIRECTINPUT, "DirectInput"},
      {INTERPOSER_FEATURE_FLAG_AUDIO, "Audio"},             {INTERPOSER_FEATURE_FLAG_OSVERSION, "OsVersion"},
      {INTERPOSER_FEATURE_FLAG_IDENTITY, "Identity"},       {INTERPOSER_FEATURE_FLAG_NETWORK, "Network"},
      {INTERPOSER_FEATURE_FLAG_RELAY, "Relay"},             {INTERPOSER_FEATURE_FLAG_RICH_PRESENCE, "RichPresence"},
      {INTERPOSER_FEATURE_FLAG_MATCHES, "Matches"},         {INTERPOSER_FEATURE_FLAG_VOICE, "Voice"},
      {INTERPOSER_FEATURE_FLAG_PLUGINS, "Plugins"},
  };
  DWORD features = InterposerGetFeatures();
  char list[256] = "";
  for (const auto& f : kFeatures) {
    if (!(features & f.flag)) continue;
    if (list[0]) strcat_s(list, ", ");
    strcat_s(list, f.name);
  }
  log::Info("interposer: %s; features: %s", g_interposerOwner ? "embedded copy owns the process"
                                                              : "another Interposer owns the process (calls forward to it)",
            list[0] ? list : "none");
}

const IMAGE_NT_HEADERS* NtHeaders(HMODULE module) {
  auto base = reinterpret_cast<const uint8_t*>(module);
  auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  return reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
}

bool IsSupportedGame(HMODULE exe) {
  const IMAGE_NT_HEADERS* nt = NtHeaders(exe);
  return nt->FileHeader.TimeDateStamp == kExpectedTimeDateStamp &&
         nt->OptionalHeader.SizeOfImage == kExpectedSizeOfImage;
}

void Initialize() {
  paths::Initialize();
  log::Open(paths::LogFile());
  config::Load(paths::ConfigFile());
  log::Info("scpatch %s starting; game dir: %ls", SCPATCH_VERSION, paths::GameDir().c_str());
  log::Info("user data root: %ls", paths::UserDataRoot().c_str());
  LogInterposer();
  crash::InstallLogger();
  features::InstallAll();
  log::Info("initialization complete");
}

// Runs in place of the game's entry point, after every statically imported DLL (including storm.dll) has
// initialized and outside the loader lock.
extern "C" void __stdcall RunDeferredInit() {
  patch::WriteMemory(g_entry, g_entryBytes, sizeof(g_entryBytes));
  Initialize();
}

__declspec(naked) void EntryStub() {
  __asm {
    pushad
    pushfd
    call RunDeferredInit
    popfd
    popad
    jmp dword ptr [g_entry]
  }
}

}  // namespace

void OnProcessAttach(HINSTANCE self) {
  HMODULE exe = GetModuleHandleW(nullptr);
  // The proxy is also picked up by other programs in the folder (StarEdit, installers); stay inert there.
  if (!IsSupportedGame(exe)) return;

  // The Interposer goes first, as its embedding contract asks: its hooks are in place before any of ours. Its
  // configuration is .interposer\Config.yml beside this DLL (the game folder), its log goes there too.
  g_interposerOwner = InterposerEmbeddedAttach(self) != FALSE;
  g_interposerAttached = true;

  g_entry = reinterpret_cast<uintptr_t>(exe) + NtHeaders(exe)->OptionalHeader.AddressOfEntryPoint;
  std::memcpy(g_entryBytes, reinterpret_cast<void*>(g_entry), sizeof(g_entryBytes));
  patch::WriteJump(g_entry, reinterpret_cast<void*>(&EntryStub));
}

void OnProcessDetach(bool processTerminating) {
  if (!g_interposerAttached) return;
  g_interposerAttached = false;
  InterposerEmbeddedDetach(processTerminating ? TRUE : FALSE);
}

}  // namespace scpatch
