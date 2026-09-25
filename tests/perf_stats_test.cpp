// ctest `perf_stats`: plugin time per frame and the gap between frames (core/src/readyup/perf_stats.h).
#include "readyup/perf_stats.h"

#include <cstdio>
#include <string>

namespace {
int g_failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      ++g_failures;                                                    \
    }                                                                  \
  } while (0)
bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }
}  // namespace

int main() {
  readyup::PerfStats p;
  p.Reset(0);
  // A normal 64-tick frame: nothing logged.
  p.Add("match", "tick", 300);
  p.Add("practice", "tick", 50);
  CHECK(p.EndFrame(0.0, 8, 100).empty());
  CHECK(p.EndFrame(0.0156, 8, 100).empty());
  // A slow frame: logged with the top callbacks, the biggest first.
  p.Add("match", "tick", 9000);
  p.Add("skins", "event:player_spawn", 1500);
  const std::string slow = p.EndFrame(0.0312, 8, 100);
  CHECK(Has(slow, "perf: slow frame: 10.50 ms in plugins"));
  CHECK(Has(slow, "top: match tick 9.00 ms, skins event:player_spawn 1.50 ms"));
  // Rate limit: another slow frame within 10 s is counted, not logged.
  p.Add("match", "tick", 20000);
  CHECK(p.EndFrame(0.05, 8, 100).empty());
  // A long gap with little plugin time: the engine, not Ready Up.
  const std::string late = p.EndFrame(20.0, 8, 100);
  CHECK(Has(late, "perf: late frame") && Has(late, "Ready Up was not the cause"));
  // The report: frames, worst frame, over-counts, the costliest row first.
  const auto r = p.Report(30.0);
  CHECK(r.size() >= 4);
  CHECK(Has(r[0], "5 frames") && Has(r[0], "worst frame 20.00 ms") && Has(r[0], "2 over the warn level"));
  CHECK(Has(r[1], "match tick") && Has(r[1], "3 calls"));
  p.Reset(30.0);
  CHECK(Has(p.Report(31.0)[1], "no plugin callbacks yet"));
  std::printf("perf_stats_test: %s\n", g_failures ? "FAIL" : "PASS");
  return g_failures ? 1 : 0;
}
