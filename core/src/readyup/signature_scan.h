#pragma once

#include <cstdint>
#include <string>

#include "readyup/engine_surface_core.h"

namespace readyup {

struct SigResult {
  void* addr = nullptr;     // first match (if any)
  int matches = 0;          // number of matches found (capped by maxMatches)
  size_t pat_len = 0;       // parsed pattern length in bytes
};

// Finds the first match of a byte-pattern in the real Valve server module's
// executable segments.
//
// Pattern format: "AA BB CC ? ? DD" (spaces optional). "?" or "??" are wildcards.
// Returns nullptr if not found.
void* FindInRealServerText(const std::string& pattern);

// Like FindInRealServerText(), but also counts matches (up to maxMatches).
SigResult FindInRealServerTextCount(const std::string& pattern, int maxMatches = 3);

// All PT_LOAD segments of the real Valve server module (runtime addresses), for
// engine-surface verification. Empty if the module isn't loaded yet.
es::Image SnapshotRealServerImage();

}  // namespace readyup

