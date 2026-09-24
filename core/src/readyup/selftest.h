#pragma once

#include <string>
#include <vector>

namespace readyup {

// `ru selftest` (server console) / `.ru selftest` (admin chat).
//
// Reports every engine-surface function (address, verified, anchors), RTTI class, vtable slot,
// struct layout, every schema field Ready Up uses (with its offset), the runtime hooks (GameFrame,
// ClientCommand, command buffer, log listener), engine events (manager / listener / delivered
// yet?), entity system, DB and clientprint status, and the feature on/off table. Ends with one
// line: `selftest: PASS n/n` or `selftest: FAIL k/n (first failures...)`.
//
// Must run on the server (GameFrame) thread or at a point where the engine is idle.
struct SelftestResult {
  bool pass = false;
  int passed = 0;
  int total = 0;
  int pending = 0;
  std::vector<std::string> lines;     // full report
  std::vector<std::string> failures;  // short names of failing checks
  std::string summary;                // "selftest: PASS 40/40"
};
SelftestResult RunSelftest(bool printToConsole);

// READYUP_SELFTEST_AND_QUIT=1 (or launch option -readyup_selftest_and_quit): after the first map
// is simulating (+ READYUP_SELFTEST_DELAY seconds, default 5), run the selftest, write the report
// to READYUP_SELFTEST_FILE (default: readyup_selftest.txt next to the shim) and quit the server
// (exit code 0 = PASS, 1 = FAIL). A watchdog quits with a FAIL report if no map is simulating
// within READYUP_SELFTEST_TIMEOUT seconds (default 300), e.g. when GameFrame could not be hooked.
bool SelftestAndQuitRequested();
void StartSelftestWatchdogIfRequested();  // call once at load
void SelftestFrameTick();                 // call from every simulating GameFrame

}  // namespace readyup
