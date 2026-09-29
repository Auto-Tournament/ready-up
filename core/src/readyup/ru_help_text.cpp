#include "readyup/ru_help_text.h"

#include <algorithm>
#include <cctype>
#include <map>

namespace readyup {

const std::vector<std::string>& CoreRuMainCommands() {
  static const std::vector<std::string> k = {"plugin", "reload", "selftest", "version", "license", "help", "list"};
  return k;
}

std::vector<std::string> RuMainHelpLines(const std::vector<std::string>& pluginMains) {
  static const std::map<std::string, std::string> kCore = {
      {"plugin", "plugins: list, load, reload, enable, disable, perf"},
      {"reload", "reload readyup.cfg"},
      {"selftest", "engine / plugin check, PASS or FAIL"},
      {"version", "the Ready Up build"},
      {"license", "commercial license key status"},
      {"list", "the commands you can use (players: version, help, list)"},
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
  if (main == "reload") {
    return {".ru reload: reload readyup.cfg; plugins re-read their settings (admin; also .reload_config, console "
            "ru_reload_config)"};
  }
  if (main == "selftest") return {".ru selftest: engine surface, hooks, features, plugins; PASS/FAIL (admin)"};
  if (main == "version") return {".ru version: the Ready Up build (also .ruversion / .version, console ru_version)"};
  if (main == "license") {
    return {".ru license: the license key's status, never blocks anything (admin; console: ru license)",
            "server.cfg: readyup_license_key \"ATL1...\" (csm license set writes it), readyup_show_license 0|1"};
  }
  if (main == "help") return {".ru help: main commands", ".ru help <command>: its subcommands"};
  if (main == "list") return {".ru list: the commands you can use (= .ru help)"};
  return {};
}

std::string RuUnknownCommandReply(const std::string& cmd) {
  return "Ready Up: unknown command \".ru " + cmd.substr(0, 32) + "\". Type .ru help for the list.";
}

namespace {

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

std::string CoreChatAliasToRu(const std::string& firstToken) {
  const std::string t = Lower(firstToken);
  if (t == ".ruversion" || t == ".version") return "version";
  if (t == ".reload_config") return "reload";
  return {};
}

std::string CoreConsoleAliasToRu(const std::string& firstToken) {
  const std::string t = Lower(firstToken);
  if (t == "ru_version") return "version";
  if (t == "ru_reload_config") return "reload";
  return {};
}

namespace {

// Plugin `.ru <main> <sub>` commands meant for every player (the plugin answers them; its admin
// subcommands stay admin-only). Add one here only for a command players have no other way to run.
struct PublicPluginCommand {
  const char* main;
  std::vector<std::string> subs;  // also open: `.ru <main>` / `.ru <main> help` (its help)
  bool alias;                     // not listed again in RuPublicHelpLines
};
const std::vector<PublicPluginCommand>& PublicPluginCommands() {
  static const std::vector<PublicPluginCommand> k = {
      // deathmatch plugin: leaderboard, status, your own leaderboard panel on / off.
      {"dm", {"top", "status", "hud"}, false},
      {"deathmatch", {"top", "status", "hud"}, true},
  };
  return k;
}

const PublicPluginCommand* FindPublicPlugin(const std::string& main) {
  for (const auto& p : PublicPluginCommands()) {
    if (main == p.main) return &p;
  }
  return nullptr;
}

}  // namespace

bool RuCommandPublic(const std::vector<std::string>& words) {
  if (words.empty()) return true;  // `.ru`: the version
  const std::string main = Lower(words[0]);
  if (main == "version" || main == "list") return true;
  if (main == "help") {
    if (words.size() < 2) return true;
    const std::string topic = Lower(words[1]);
    return topic == "version" || topic == "list" || topic == "help" || FindPublicPlugin(topic) != nullptr;
  }
  if (const PublicPluginCommand* p = FindPublicPlugin(main)) {
    if (words.size() < 2) return true;  // its help
    const std::string sub = Lower(words[1]);
    if (sub == "help") return true;
    return std::find(p->subs.begin(), p->subs.end(), sub) != p->subs.end();
  }
  return false;
}

std::vector<std::string> RuPublicHelpLines(const std::vector<std::string>& pluginMains) {
  std::vector<std::string> out = {"Ready Up commands:", ".ru version: the Ready Up build",
                                  ".ru help (.ru list): the commands you can use"};
  std::map<std::string, bool> seen;
  for (const auto& e : pluginMains) {
    const std::string name = e.substr(0, e.find(' '));
    const PublicPluginCommand* p = FindPublicPlugin(name);
    if (!p || p->alias || seen[name]) continue;
    seen[name] = true;
    std::string l = ".ru " + name + " ";
    for (size_t i = 0; i < p->subs.size(); ++i) l += (i ? "|" : "") + p->subs[i];
    out.push_back(l + " (.ru help " + name + ")");
  }
  out.push_back("Players: .help");
  return out;
}

}  // namespace readyup
