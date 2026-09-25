#include "readyup/admin_commands.h"

#include "readyup/map_names.h"

namespace readyup {

const std::vector<AdminCommandHelp>& AdminCommandsHelp() {
  static const std::vector<AdminCommandHelp> k = {
      {".ru map <name|workshop id>", "change map"},
      {".ru reloadmap", "load the current map again"},
      {".ru restart", "restart the game (mp_restartgame 1)"},
      {".ru load <url>", "load a match config"},
      {".ru end", "end the loaded match and reset the server"},
      {".ru match restart", "the loaded match back to warmup"},
  };
  return k;
}

std::string AdminCommandsChatLine() {
  std::string out;
  for (const auto& c : AdminCommandsHelp()) out += (out.empty() ? "" : " | ") + std::string(c.usage);
  return out;
}

bool ParseMapCommand(const std::vector<std::string>& args, std::string* entry, std::string* err) {
  if (args.size() != 1) {
    if (err) *err = "usage: .ru map <name|workshop id>";
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

bool ParseLoadCommand(const std::vector<std::string>& args, std::string* url, std::string* err) {
  if (args.size() != 1) {
    if (err) *err = "usage: .ru load <url>";
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
