#pragma once

// Where the server frame goes: time spent in each plugin callback (ticks, frames, events, tasks,
// commands), per game frame, and the gap between simulated frames. `ru perf` prints it; a frame
// whose plugin time or gap is over the thresholds is logged with its top callbacks. Pure
// bookkeeping (ctest `perf_stats`); the loader feeds it on the game thread.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace readyup {

class PerfStats {
 public:
  // One plugin callback took `us` microseconds. `what` is its kind ("tick", "event", a command…).
  void Add(const std::string& plugin, const std::string& what, double us);

  // A simulated frame ended at `nowS` (seconds, monotonic). Returns a log line when the frame's
  // plugin time is over `warnMs` or the gap since the previous simulated frame is over `gapWarnMs`
  // (at most one line per `minLogGapS`), else "".
  std::string EndFrame(double nowS, double warnMs, double gapWarnMs, double minLogGapS = 10.0);

  // `ru perf`: totals since the last reset, the costliest callbacks first.
  std::vector<std::string> Report(double nowS, size_t maxRows = 12) const;
  void Reset(double nowS);

 private:
  struct Row {
    uint64_t calls = 0;
    double totalUs = 0;
    double maxUs = 0;
  };
  static constexpr size_t kMaxRows = 256;
  std::map<std::string, Row> rows_;          // "plugin what"
  std::map<std::string, double> thisFrame_;  // "plugin what" -> us in the current frame
  double frameUs_ = 0;
  uint64_t frames_ = 0, slowFrames_ = 0, gaps_ = 0;
  double maxFrameUs_ = 0, maxGapMs_ = 0;
  double lastFrameS_ = -1, lastLogS_ = -1e18, sinceS_ = 0;
};

}  // namespace readyup
