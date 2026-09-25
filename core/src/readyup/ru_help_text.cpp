#include "readyup/ru_help_text.h"

#include <algorithm>
#include <map>

namespace readyup {

const std::vector<std::string>& CoreRuMainCommands() {
  static const std::vector<std::string> k = {"plugin", "reload", "selftest", "version", "help"};
  return k;
}

std::vector<std::string> RuMainHelpLines(const std::vector<std::string>& pluginMains) {
  static const std::map<std::string, std::string> kCore = {
      {"plugin", "plugins: list, load, reload, enable, disable, perf"},
      {"reload", "reload readyup.cfg"},
      {"selftest", "engine / plugin check, PASS or FAIL"},
      {"version", "the Ready Up build"},
  };
  std::map<std::string, std::string> all;  // name -> description, sorted
  for (const auto& kv : kCore) all[kv.first] = kv.second;
  for (const auto& e : pluginMains) {
    const size_t sp = e.find(' ');
    const std::string name = e.substr(0, sp);
    if (name.empty() || all.count(name)) continue;
    std::string owner = sp == std::string::npos ? std::string() : e.substr(sp + 1);
    if (owner.size() >= 2 && owner.front() == '(' && owner.back() == ')') owner = owner.substr(1, owner.size() - 2);
    all[name] = owner.empty() ? std::string("plugin") : owner + " plugin";
  }
  std::vector<std::string> out = {"Ready Up commands (.ru help <command> for its subcommands):"};
  for (const auto& kv : all) out.push_back(".ru " + kv.first + ": " + kv.second);
  out.push_back("Players: .help");
  return out;
}

std::vector<std::string> CoreRuSubHelpLines(const std::string& main) {
  if (main == "plugin" || main == "plugins") {
    return {".ru plugin: Ready Up plugins (admin)", ".ru plugin list: loaded plugins",
            ".ru plugin load|unload|reload <name>: csgo/readyup/plugins/<name>.so, until a restart",
            ".ru plugin enable|disable <name>: load / unload it and keep it that way after a restart",
            ".ru plugin perf [reset]: time each plugin takes per server frame (console: ru perf)"};
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
