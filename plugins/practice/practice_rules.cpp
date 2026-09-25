#include "practice_rules.h"

#include <cctype>
#include <set>

namespace practice {
namespace {
std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
std::string Trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}
}  // namespace

bool IsToolCommand(const std::string& cmd) {
  static const std::set<std::string> k = {".rethrow", ".rt",   ".savepos", ".loadpos", ".back",  ".clear",
                                          ".noflash", ".god", ".spawn",   ".ctspawn", ".tspawn"};
  return k.count(Lower(cmd)) != 0;
}

bool IsBotCommand(const std::string& cmd) {
  static const std::set<std::string> k = {".bot", ".cbot", ".crouchbot", ".boost", ".crouchboost", ".nobots"};
  return k.count(Lower(cmd)) != 0;
}

bool ToolsAllowed(bool practiceActive, const std::string& ruleset) {
  return practiceActive && Lower(Trim(ruleset)) != "valve";
}

bool MatchBlocksPractice(const std::string& ruMode) {
  return ruMode == "match_warmup" || ruMode == "match_knife" || ruMode == "match_live" || ruMode == "knife" ||
         ruMode == "postgame";
}

bool ShouldAutoEnter(bool always, bool practiceActive, const std::string& ruMode) {
  return always && !practiceActive && !MatchBlocksPractice(ruMode);
}

bool ParseBool(const std::string& text, bool def) {
  const std::string t = Lower(Trim(text));
  if (t == "1" || t == "true" || t == "yes" || t == "on") return true;
  if (t == "0" || t == "false" || t == "no" || t == "off") return false;
  return def;
}

const char* HelpLine() {
  return "Ready Up practice: .bot .cbot .nobots | .savepos/.loadpos [name] .back | .spawn/.ctspawn/.tspawn N | "
         ".rethrow .clear .noflash .god | .prac to leave";
}

}  // namespace practice
