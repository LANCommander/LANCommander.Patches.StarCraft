#pragma once

#include <windows.h>

namespace scpatch {

// Called from DllMain. Verifies the host process and defers real initialization to the game's entry point.
// In StarCraft 1.16.1 it also attaches the embedded LANCommander Interposer, before the patch installs anything.
void OnProcessAttach(HINSTANCE self);
// DLL_PROCESS_DETACH: detaches the Interposer if it was attached.
void OnProcessDetach(bool processTerminating);

}  // namespace scpatch
