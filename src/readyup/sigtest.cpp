#include "readyup/sigtest.h"

#include "readyup/config.h"
#include "readyup/engine_surface.h"
#include "readyup/logging.h"

#include <cstdlib>
#include <string>

namespace readyup {

// Verifies every function in gamedata/engine-surface.json against the running
// libserver.so: exactly one signature match AND every identity anchor passes
// (see engine_surface_core.h). Only functions marked "required" fail the test;
// optional ones just stay unresolved and their feature disables itself.
//
// Required set (engine-surface.json): UTIL_ClientPrintAll. Host_Say and
// CCSGameRules_TerminateRound are optional so a broken hook degrades one
// feature instead of taking all of ReadyUp down.
bool RunSigTest(bool verbose) {
  const es::EngineSurface* s = GetEngineSurface();
  if (!s) {
    PrintLine("sigtest FAIL: engine-surface.json unavailable");
    return false;
  }

  bool ok = true;
  for (const auto& f : s->functions) {
    const es::Resolution r = EngineFunctionResolution(f.name.c_str());
    if (!r.ok) {
      Print("sigtest %s %s: %s\n", f.required ? "FAIL" : "WARN", f.name.c_str(), r.detail.c_str());
      if (f.required) ok = false;
      continue;
    }
    if (verbose) Print("sigtest OK   %s: %p (%s)\n", f.name.c_str(), reinterpret_cast<void*>(r.addr), r.detail.c_str());
  }
  return ok;
}

void RunSigTestOrDie() {
  if (RunSigTest(/*verbose=*/false)) return;

  // Hard-fail to avoid undefined behavior/hooking wrong functions.
  PrintLine("sigtest: hard-failing ReadyUp load due to signature mismatch.");
  std::abort();
}

}  // namespace readyup
