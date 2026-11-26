#pragma once

#include <string>

namespace readyup {

// Which console / log lines start a map (RU_EVENT_MAP_START): one per level load, a reload of
// the same map included. CS2 prints "SV:  Level loading started for '<map>'" once per load and
// logs `L ...: Loading map "<map>"` after it; a `map` at boot also prints a plain
// `Loading map "<map>"` before that load's level line. Pure: tests/map_load_lines_test.cpp.
class MapLoadLines {
 public:
  struct Start {
    bool start = false;     // this line starts a map
    bool new_load = false;  // a load of its own: sent even when the map did not change
    std::string map;
  };

  // `currentMap` is the map of the last map start sent ("" before the first).
  Start Observe(const std::string& line, const std::string& currentMap);

 private:
  std::string levelLineDue_;  // a map start sent from "Loading map" before this load's level line
};

}  // namespace readyup
