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
  const auto lines = RuMainHelpLines({"match", "map", "mode", "admins", "hud", "match"});
  CHECK(lines.size() == 2);
  CHECK(lines[0] ==
        "Ready Up commands: .ru admins | .ru hud | .ru map | .ru match | .ru mode | .ru plugin | .ru reload | "
        ".ru selftest | .ru version");
  CHECK(Has(lines[1], ".ru help <command>"));
  CHECK(RuMainHelpLines({})[0] == "Ready Up commands: .ru plugin | .ru reload | .ru selftest | .ru version");

  CHECK(CoreRuSubHelpLines("plugin").size() == 3 && Has(CoreRuSubHelpLines("plugin")[2], "reload <name>"));
  CHECK(!CoreRuSubHelpLines("reload").empty() && !CoreRuSubHelpLines("selftest").empty());
  CHECK(CoreRuSubHelpLines("match").empty());  // a plugin's: forwarded as `.ru match help`

  CHECK(RuUnknownCommandReply("restart") == "Ready Up: unknown command \".ru restart\". Type .ru help for the list.");
  std::printf("ru_help_test: %s\n", g_failures ? "FAIL" : "PASS");
  return g_failures ? 1 : 0;
}
