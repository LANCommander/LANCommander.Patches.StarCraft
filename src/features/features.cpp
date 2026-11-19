#include "features/features.h"

namespace scpatch::features {

void InstallAll() {
  InstallSavePaths();
  InstallRenderer();
  InstallNetwork();  // after the renderer: chains to its LoadLibraryA hook in storm.dll
  InstallFramePacing();
  InstallWidescreen();
  InstallZoom();
  InstallSelection();
  InstallRally();
  InstallControls();
  InstallRules();  // after the features whose rules it carries
  InstallMatches();  // reads the rules (hosting, the agreed rules) when a game starts
}

}  // namespace scpatch::features
