// Offline tests for core/src/readyup/ru_help_text.h: `.ru help` lists main commands (core +
// plugins), `.ru help <core command>` its subcommands, the unknown-command reply, and who may run
// `.ru ...` (RuCommandPublic / RuCommandAllowed: admin-only unless public). ctest `ru_help`.
#include "readyup/ru_help_text.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace readyup;

static int g_failures = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

static bool Has(const std::string& s, const std::string& n) { return s.find(n) != std::string::npos; }

int main() {
  const auto lines = RuMainHelpLines({"match (match)", "map (match)", "practice (practice)", "hello", "match (match)"});
  CHECK(lines.size() == 1 + 10 + 1);  // header, 6 core + 4 plugin mains (match once), players line
  CHECK(Has(lines[0], ".ru help <command>"));
  CHECK(lines[1] == ".ru hello: plugin");
  CHECK(lines[2] == ".ru license: commercial license key status");
  CHECK(Has(lines[3], ".ru list: "));
  CHECK(lines[4] == ".ru map: match plugin");
  CHECK(lines[5] == ".ru match: match plugin");
  CHECK(lines[6] == ".ru plugin: plugins: list, load, reload, enable, disable, perf");
  CHECK(lines[7] == ".ru practice: practice plugin");
  CHECK(lines[8] == ".ru reload: reload readyup.cfg");
  CHECK(lines.back() == "Players: .help");
  CHECK(RuMainHelpLines({}).size() == 1 + 6 + 1);
  for (const auto& l : lines) CHECK(l.size() < 120);  // one short chat line each

  CHECK(CoreRuSubHelpLines("plugin").size() == 5 && Has(CoreRuSubHelpLines("plugin")[2], "reload <name>"));
  CHECK(Has(CoreRuSubHelpLines("plugin")[3], "enable|disable <name>"));
  CHECK(Has(CoreRuSubHelpLines("plugin")[4], "perf [reset]"));
  CHECK(!CoreRuSubHelpLines("reload").empty() && !CoreRuSubHelpLines("selftest").empty());
  CHECK(CoreRuSubHelpLines("license").size() == 2 && Has(CoreRuSubHelpLines("license")[0], "never blocks"));
  CHECK(CoreRuSubHelpLines("match").empty());  // a plugin's: forwarded as `.ru match help`

  CHECK(RuUnknownCommandReply("restart") == "Ready Up: unknown command \".ru restart\". Type .ru help for the list.");
  // ---- who may run `.ru ...` (default deny for players) --------------------------------------
  using W = std::vector<std::string>;
  // Public: `.ru`, version, help, list (any case), help for a public topic.
  for (const W& w : {W{}, W{"version"}, W{"VERSION"}, W{"help"}, W{"Help"}, W{"list"}, W{"list", "x"},
                     W{"help", "version"}, W{"help", "list"}, W{"help", "dm"}, W{"help", "DM"}}) {
    CHECK(RuCommandPublic(w));
    CHECK(RuCommandAllowed(w, false));
  }
  // Deathmatch player commands (no chat alias): top, status, hud, and its help.
  for (const W& w : {W{"dm"}, W{"dm", "help"}, W{"dm", "top"}, W{"dm", "status"}, W{"DM", "HUD"},
                     W{"deathmatch", "top"}, W{"deathmatch", "hud", "extra"}}) {
    CHECK(RuCommandAllowed(w, false));
  }
  // Everything else is admin-only: core commands, every other plugin command, unknown ones, and
  // help for an admin topic.
  for (const W& w : {W{"plugin", "list"}, W{"plugins"}, W{"reload"}, W{"selftest"}, W{"license"}, W{"sigtest"},
                     W{"status_http"}, W{"match", "state"}, W{"match", "load", "http://x"}, W{"MATCH", "end"},
                     W{"mode", "show"}, W{"settings", "show"}, W{"hud", "test", "1"}, W{"admins"},
                     W{"admins", "add", "1"}, W{"map", "change", "de_dust2"}, W{"map", "defaults"},
                     W{"whitelist", "list"}, W{"whitelist", "help"}, W{"practice", "status"}, W{"addons"},
                     W{"as", "1", ".gg"}, W{"dm", "ffa"}, W{"dm", "off"}, W{"deathmatch", "tdm"}, W{"fleet", "status"},
                     W{"r"}, W{"nosuchcommand"}, W{"help", "match"}, W{"help", "plugin"}, W{"help", "admins"}}) {
    CHECK(!RuCommandPublic(w));
    CHECK(!RuCommandAllowed(w, false));
    CHECK(RuCommandAllowed(w, true));  // admins may run everything
  }

  // `.ru help` for a player who is not an admin: only what they can run.
  const auto pub = RuPublicHelpLines({"match (match)", "dm (deathmatch)", "deathmatch (deathmatch)", "admins (essentials)"});
  CHECK(pub.size() == 5);  // header, version, help/list, dm (once), players line
  CHECK(Has(pub[1], ".ru version"));
  CHECK(Has(pub[2], ".ru help") && Has(pub[2], ".ru list"));
  CHECK(pub[3] == ".ru dm top|status|hud (.ru help dm)");
  CHECK(pub.back() == "Players: .help");
  for (const auto& l : pub) CHECK(!Has(l, "match") && !Has(l, "admins") && !Has(l, "plugin"));
  CHECK(RuPublicHelpLines({}).size() == 4);  // no deathmatch plugin: no dm line
  CHECK(!CoreRuSubHelpLines("list").empty());

  // The old plugin's names (MatchZy Enhanced / AT: .version, matchzy_version, at_reload_config).
  CHECK(CoreChatAliasToRu(".ruversion") == "version" && CoreChatAliasToRu(".Version") == "version");
  CHECK(CoreChatAliasToRu(".reload_config") == "reload");
  CHECK(CoreChatAliasToRu(".ru").empty() && CoreChatAliasToRu(".versions").empty() && CoreChatAliasToRu("").empty());
  CHECK(CoreConsoleAliasToRu("ru_version") == "version" && CoreConsoleAliasToRu("RU_RELOAD_CONFIG") == "reload");
  CHECK(CoreConsoleAliasToRu("ru").empty() && CoreConsoleAliasToRu(".version").empty());
  CHECK(Has(CoreRuSubHelpLines("version")[0], "ru_version") && Has(CoreRuSubHelpLines("reload")[0], "ru_reload_config"));

  std::printf("ru_help_test: %s\n", g_failures ? "FAIL" : "PASS");
  return g_failures ? 1 : 0;
}
