#include "readyup/ru_help_text.h"

#include <algorithm>

namespace readyup {

const std::vector<std::string>& CoreRuMainCommands() {
  static const std::vector<std::string> k = {"plugin", "reload", "selftest", "version", "help"};
  return k;
}

std::vector<std::string> RuMainHelpLines(const std::vector<std::string>& pluginMains) {
  std::vector<std::string> all = pluginMains;
  for (const auto& c : CoreRuMainCommands()) {
    if (c != "help") all.push_back(c);
  }
  std::sort(all.begin(), all.end());
  all.erase(std::unique(all.begin(), all.end()), all.end());
  std::string line = "Ready Up commands:";
  for (const auto& c : all) line += " .ru " + c + " |";
  line.pop_back();
  while (!line.empty() && line.back() == ' ') line.pop_back();
  return {line, "Type .ru help <command> for its subcommands (e.g. .ru help match). Players: .help"};
}

std::vector<std::string> CoreRuSubHelpLines(const std::string& main) {
  if (main == "plugin" || main == "plugins") {
    return {".ru plugin: Ready Up plugins (admin)", ".ru plugin list: loaded plugins",
            ".ru plugin load|unload|reload <name>: csgo/readyup/plugins/<name>.so"};
  }
  if (main == "reload") return {".ru reload: reload readyup.cfg; plugins re-read their settings (admin)"};
  if (main == "selftest") return {".ru selftest: engine surface, hooks, features, plugins; PASS/FAIL (admin)"};
  if (main == "version") return {".ru version: the Ready Up build"};
  if (main == "help") return {".ru help: main commands", ".ru help <command>: its subcommands"};
  return {};
}

std::string RuUnknownCommandReply(const std::string& cmd) {
  return "Ready Up: unknown command \".ru " + cmd.substr(0, 32) + "\". Type .ru help for the list.";
}

}  // namespace readyup
