#pragma once

#include <optional>
#include <string>

namespace readyup {

struct Cs2VersionSnapshot {
  std::optional<long long> build_id;
  std::optional<std::string> version_string;
};

// Best-effort: returns CS2 build/version info by parsing steam.inf.
// This is cached to avoid repeated disk I/O (safe to call frequently).
Cs2VersionSnapshot GetCs2VersionSnapshot();

}  // namespace readyup

