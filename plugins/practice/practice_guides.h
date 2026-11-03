#pragma once

// readyup-practice guides: lineups read from CS2 map annotation files (the KV3 text files the
// game's own "guide" feature loads from a Workshop item: MapAnnotationNode0 = { Type = "grenade"
// SubType = "main" ... }). Engine-free (ctest `practice_rules`).
//
// A grenade guide is three nodes tied by MasterNodeId: "main" (where to stand), "aim_target"
// (where to look: its Angles, and its Desc says how to throw) and "destination" (where it lands).
// Other node types (line, spot, text, position) are not lineups and are skipped.
//
// The shipped files live in plugins/practice/guides/ (credited in its README.md); the plugin reads
// <data dir>/guides/*.txt and keeps the ones whose MapName is the current map.

#include "practice_lineups.h"

#include <string>
#include <vector>

namespace practice {

constexpr const char* kGuideOwner = "guide";

struct GuideFile {
  std::string map;               // MapName
  std::vector<Lineup> lineups;   // name: a slug of the title; title: as written; desc: the throw
  int skipped = 0;               // grenade guides without a usable stand spot
};

// Parses one annotation file. False (with *error) when the text is not KV3 the parser can read.
bool ParseGuideFile(const std::string& text, GuideFile* out, std::string* error);

// "[INSTA] T window smoke" -> "insta-t-window-smoke" (ValidLineupName: at most 32 characters).
std::string GuideSlug(const std::string& title);

// How to throw, from the first line of an aim_target's Desc ("run + jumpthrow" -> "W + jumpthrow",
// "lmb + rmb" -> "Middle throw", "shift + jumpthrow" -> "Shift + jumpthrow"); "" when it only says
// where to aim. `jumpFlag`: the main node's JumpThrow.
std::string GuideThrow(const std::string& desc, bool jumpFlag);

// The grenade kind practice uses: "smoke", "flash", "molotov" (incendiary too), "hegrenade".
std::string GuideKind(const std::string& grenadeType);

}  // namespace practice
