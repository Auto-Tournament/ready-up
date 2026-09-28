#include "readyup/ru_commands.h"

#include "readyup/map_names.h"

namespace readyup {

const std::vector<RuMainCommand>& MatchRuCommands() {
  static const std::vector<RuMainCommand> k = {
      {"match",
       "the loaded match",
       {
           {"load", "<url>", "load a match config", true},
           {"start", "[force]", "force-start the loaded match (force: valve without GOTV)", true},
           {"restart", "", "the loaded match back to warmup (everyone readies again)", true},
           {"end", "", "end the loaded match (winner none) and reset the server", true},
           {"recover", "[round]", "ask the platform to recover the match", true},
           {"restore", "<round>", "play <round> of this map again from its start (also .restore)", true},
           {"backups", "", "this match's round backups on the server", true},
           {"pause", "", "admin pause", true},
           {"unpause", "", "unpause", true},
           {"tech", "team1|team2", "technical pause for a team (its limits)", true},
           {"tac", "team1|team2", "tactical timeout for a team (its limits)", true},
           {"swap", "", "swap the teams' sides in warmup; team1 / team2 stay who they are (also .switch)", true},
           {"team1", "<name>", "rename team1 (also .team1)", true},
           {"team2", "<name>", "rename team2 (also .team2)", true},
           {"side", "stay|switch|ct|t", "knife side pick (the knife-winning team, or an admin)", false},
           {"coach", "<player> team1|team2|ct|t", "make a spectator a team's coach (players: .coach ct|t)", true},
           {"uncoach", "<player>", "stop a player coaching (players: .uncoach)", true},
           {"state", "", "match and mode state", false},
           {"rules", "", "effective rules (ruleset + overrides)", false},
       }},
      {"mode",
       "server mode",
       {
           {"show", "", "the current mode", false},
           {"idle", "", "plain CS2; no auto scrim warmup until `mode scrim` or a map change", true},
           {"practice", "", "practice mode on / off (practice plugin; also .prac)", true},
           {"scrim", "", "auto scrim warmup back on", true},
       }},
      {"settings",
       "server settings (also console ru_<setting>)",
       {
           {"show", "", "every setting and its value (also .settings)", false},
           {"set", "<setting> <value>", "change one; saved across restarts", true},
           {"default", "<setting>", "back to readyup.cfg / the built-in default", true},
       }},
      {"hud",
       "center-screen HUD",
       {
           {"test", "<1-11>", "show a HUD test panel to you (10 s)", true},
           {"anim", "[hz] [seconds]", "animation test: an eased bar redrawn hz times a second, with a frame counter (record it to count the fps)", true},
       }},
  };
  return k;
}

const RuMainCommand* FindRuMain(const std::string& name) {
  for (const auto& m : MatchRuCommands()) {
    if (name == m.name) return &m;
  }
  return nullptr;
}

const RuSubcommand* FindRuSub(const RuMainCommand& main, const std::string& sub) {
  for (const auto& s : main.subs) {
    if (sub == s.name) return &s;
  }
  return nullptr;
}

std::vector<std::string> RuHelpLines(const RuMainCommand& main) {
  std::vector<std::string> out;
  out.push_back(std::string(".ru ") + main.name + ": " + main.help);
  for (const auto& s : main.subs) {
    std::string l = std::string(".ru ") + main.name + " " + s.name;
    if (*s.args) l += std::string(" ") + s.args;
    l += std::string(": ") + s.help;
    if (s.admin) l += " (admin)";
    out.push_back(std::move(l));
  }
  return out;
}

std::string RuUnknownSubReply(const std::string& main, const std::string& sub) {
  return "Ready Up: unknown command \".ru " + main + " " + sub.substr(0, 32) + "\". Type .ru help " + main +
         " for the list.";
}

bool ParseMapChange(const std::vector<std::string>& args, std::string* entry, std::string* err) {
  if (args.size() != 1) {
    if (err) *err = "usage: .ru map change <name|workshop id>";
    return false;
  }
  const std::string& a = args[0];
  if (!mapnames::ValidEntry(a)) {
    if (err) *err = "\"" + a.substr(0, 64) + "\" is not a map name or workshop id";
    return false;
  }
  if (entry) *entry = a;
  return true;
}

std::string RestAfterWords(const std::string& line, size_t n) {
  auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  size_t i = 0;
  for (size_t w = 0; w < n; ++w) {
    while (i < line.size() && blank(line[i])) ++i;
    if (i >= line.size()) return {};
    while (i < line.size() && !blank(line[i])) ++i;
  }
  while (i < line.size() && blank(line[i])) ++i;
  size_t e = line.size();
  while (e > i && blank(line[e - 1])) --e;
  return line.substr(i, e - i);
}

bool ParseMatchLoad(const std::vector<std::string>& args, std::string* url, std::string* err) {
  if (args.size() != 1) {
    if (err) *err = "usage: .ru match load <url>";
    return false;
  }
  const std::string& a = args[0];
  if (a.rfind("http://", 0) != 0 && a.rfind("https://", 0) != 0) {
    if (err) *err = "the match config URL must start with http:// or https://";
    return false;
  }
  if (url) *url = a;
  return true;
}

}  // namespace readyup
