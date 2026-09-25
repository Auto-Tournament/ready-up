// Offline tests for core/src/readyup/ru_help_text.h: `.ru help` lists main commands (core +
// plugins), `.ru help <core command>` its subcommands, the unknown-command reply. ctest `ru_help`.
#include "readyup/ru_help_text.h"

#include <cstdio>
#include <string>

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
  CHECK(lines.size() == 1 + 8 + 1);  // header, 4 core + 4 plugin mains (match once), players line
  CHECK(Has(lines[0], ".ru help <command>"));
  CHECK(lines[1] == ".ru hello: plugin");
  CHECK(lines[2] == ".ru map: match plugin");
  CHECK(lines[3] == ".ru match: match plugin");
  CHECK(lines[4] == ".ru plugin: plugins: list, load, reload, enable, disable");
  CHECK(lines[5] == ".ru practice: practice plugin");
  CHECK(lines[6] == ".ru reload: reload readyup.cfg");
  CHECK(lines.back() == "Players: .help");
  CHECK(RuMainHelpLines({}).size() == 1 + 4 + 1);
  for (const auto& l : lines) CHECK(l.size() < 120);  // one short chat line each

  CHECK(CoreRuSubHelpLines("plugin").size() == 4 && Has(CoreRuSubHelpLines("plugin")[2], "reload <name>"));
  CHECK(Has(CoreRuSubHelpLines("plugin")[3], "enable|disable <name>"));
  CHECK(!CoreRuSubHelpLines("reload").empty() && !CoreRuSubHelpLines("selftest").empty());
  CHECK(CoreRuSubHelpLines("match").empty());  // a plugin's: forwarded as `.ru match help`

  CHECK(RuUnknownCommandReply("restart") == "Ready Up: unknown command \".ru restart\". Type .ru help for the list.");
  std::printf("ru_help_test: %s\n", g_failures ? "FAIL" : "PASS");
  return g_failures ? 1 : 0;
}
