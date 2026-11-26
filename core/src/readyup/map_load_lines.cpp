#include "readyup/map_load_lines.h"

#include <cstring>

namespace readyup {

static bool Between(const std::string& s, size_t from, const char* open, char close, std::string* out) {
  const size_t a = s.find(open, from);
  if (a == std::string::npos) return false;
  const size_t start = a + std::strlen(open);
  const size_t b = s.find(close, start);
  if (b == std::string::npos || b == start) return false;
  *out = s.substr(start, b - start);
  return true;
}

MapLoadLines::Start MapLoadLines::Observe(const std::string& line, const std::string& currentMap) {
  Start r;
  const size_t level = line.find("Level loading started for '");
  if (level != std::string::npos) {
    if (!Between(line, level, "for '", '\'', &r.map)) return Start{};
    const bool sent = levelLineDue_ == r.map;  // the boot `map` line sent this load already
    levelLineDue_.clear();
    if (sent) return Start{};
    r.start = r.new_load = true;
    return r;
  }
  size_t at = line.find("Loading map \"");
  if (at == std::string::npos) at = line.find("Started map \"");
  if (at == std::string::npos) return Start{};
  if (!Between(line, at, "map \"", '"', &r.map)) return Start{};
  // The same map again is a line of the load already sent. A new map starts here (also where
  // the level line is never seen, e.g. the log file reader).
  if (r.map == currentMap) return Start{};
  levelLineDue_ = r.map;
  r.start = true;
  return r;
}

}  // namespace readyup
