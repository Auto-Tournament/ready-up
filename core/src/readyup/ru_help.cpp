#include "readyup/ru_help.h"

#include "readyup/logging.h"
#include "readyup/plugin_loader.h"
#include "readyup/version.h"

namespace readyup {

void PrintRuHelp() {
  static constexpr const char* kBanner = R"(
 ______     ______     ______     _____     __  __        __  __     ______  
╱╲  == ╲   ╱╲  ___╲   ╱╲  __ ╲   ╱╲  __─.  ╱╲ ╲_╲ ╲      ╱╲ ╲╱╲ ╲   ╱╲  == ╲ 
╲ ╲  __<   ╲ ╲  __╲   ╲ ╲  __ ╲  ╲ ╲ ╲╱╲ ╲ ╲ ╲____ ╲     ╲ ╲ ╲_╲ ╲  ╲ ╲  _─╱ 
 ╲ ╲_╲ ╲_╲  ╲ ╲_____╲  ╲ ╲_╲ ╲_╲  ╲ ╲____─  ╲╱╲_____╲     ╲ ╲_____╲  ╲ ╲_╲   
  ╲╱_╱ ╱_╱   ╲╱_____╱   ╲╱_╱╲╱_╱   ╲╱____╱   ╲╱_____╱      ╲╱_____╱   ╲╱_╱   
)";

  // PrintRaw formats into a 2048-byte buffer; keep each call well below that.
  PrintRaw("%s\n", kBanner);
  PrintRaw(
      "\n"
      "[ReadyUp] build: %s\n"
      "Author: Sivert Gullberg Hansen\n"
      "Repo:   github.com/Auto-Tournament/ready-up\n"
      "\n"
      "Core console / RCON commands:\n"
      "  - ru / ru help            (prints this help)\n"
      "  - ru help <command>       (the subcommands of a main command, e.g. ru help match)\n"
      "  - ru version\n"
      "  - ru reload               (reload readyup.cfg; plugins re-read their own settings)\n"
      "  - ru selftest             (engine surface, hooks, schema, events, plugins, features; PASS/FAIL)\n"
      "  - ru sigtest\n"
      "  - ru status_http          (local status endpoint address + counters)\n"
      "  - ru plugin list\n"
      "  - ru plugin load|unload|reload <name>   (csgo/readyup/plugins/<name>.so)\n"
      "\n"
      "Core chat commands (admins): .ru plugin ... | .ru reload | .ru selftest | .ru version | .ru help\n"
      "\n"
      "The match flow (ready-up, scrims, knife, pauses, practice, `ru match load`, webhooks, demos)\n"
      "is the match plugin (plugins/match, match.so); its main commands are listed below when it is\n"
      "loaded (`ru help <command>` for their subcommands).\n",
      BuildVersion());
  const auto subs = plugins::PluginRuSubcommands();
  if (!subs.empty()) {
    PrintRaw("\nPlugin main commands (ru <command> <sub> / .ru <command> <sub>):\n");
    for (const auto& s : subs) PrintRaw("  - ru %s\n", s.c_str());
  }
  const auto cmds = plugins::PluginCommandSummary();
  if (!cmds.empty()) {
    PrintRaw("\nPlugin chat / console commands:\n");
    for (const auto& c : cmds) PrintRaw("  - %s\n", c.c_str());
  }
}

}  // namespace readyup
