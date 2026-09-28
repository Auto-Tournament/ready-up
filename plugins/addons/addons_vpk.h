#pragma once
// Reading a Source 2 VPK (v1 / v2 directory + numbered archives), no engine involved. The addons
// plugin extracts an addon's content files as loose files into Ready Up's own `Game csgo/readyup`
// search path: the engine's addon mount only exposes them to clients, not to the server's resource
// system ("Failed loading resource ... File not found" on precache).
#include <cstdint>
#include <string>
#include <vector>

namespace addons {

struct VpkEntry {
  std::string path;  // "models/midas/weapon_rif_ak47_gold.vmdl_c"
  std::string preload;
  uint16_t archive = 0;  // 0x7FFF = data in the _dir file after the tree
  uint32_t offset = 0;
  uint32_t length = 0;
};

struct VpkDir {
  std::vector<VpkEntry> entries;
  uint32_t dataStart = 0;  // offset of the data after the tree in the _dir file (archive 0x7FFF)
};

// Parses a _dir.vpk. False (and *error) for a truncated / non-VPK file.
bool ParseVpkDir(const std::string& bytes, VpkDir* out, std::string* error);

// Only content goes into csgo/readyup: models/, materials/, sounds/, soundevents/, particles/,
// panorama/images/. Nothing absolute, no "..", nothing over Ready Up's own folders.
bool ExtractablePath(const std::string& path);

}  // namespace addons
