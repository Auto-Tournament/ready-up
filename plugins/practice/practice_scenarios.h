#pragma once

// readyup-practice scenarios ("pro round replay"), engine side. See docs/SCENARIOS.md.
//
//   .scen / .scenario list                         scenarios for this map (.scen list all: every map)
//   .scen load <id> [player] [start]               play <player> (name / number / "watch" = bots only)
//                                                  from <start> (25, 1:05, t12345; default the
//                                                  scenario's start); bots re-enact everyone else
//   .scen restart (.scen r)                        the same scenario again, from the same start
//   .scen stop                                     end it: bots kicked, prac.cfg back
//   .scen info <id>                                players, length, grenades, source
//   ru scenario ... (console / .ru scenario)       the same, plus `status`, `rounds <demo>` and
//                                                  `convert <demo> <round[,round]> [start] [id]`
//                                                  (runs tools/scenario/ru_scenario.py on a demo
//                                                  that is on the server)
//
// Only in practice mode, never under the valve ruleset. Scenario files: <data_dir>/scenarios/*.json
// (csgo/readyup/plugins/practice/scenarios/). Grenades are thrown with ru_api grenade_spawn (1.12);
// without it (older core, or that grenade's gamedata did not verify) they are logged and skipped.

#include "readyup/plugin_api.h"
#include "readyup/selftest_iface.h"

#include <string>

namespace practice::scenarios {

struct Host {
  bool (*active)();          // practice mode is on
  std::string (*refusal)();  // "" when practice tools may run, else why not
};

// From readyup_plugin_load / readyup_plugin_unload. Registers its own commands, tick and events.
void Load(const ru_api* api, const Host& host);
void Unload();
// Lines for the practice plugin's `ru selftest` section (any thread; reads atomics only).
void Selftest(ru_selftest_add_fn add, void* ctx);

}  // namespace practice::scenarios
