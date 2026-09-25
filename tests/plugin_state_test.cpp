// Offline tests for core/src/readyup/plugin_state.h: readyup/plugins.json (which plugins stay
// off across restarts). ctest `plugin_state`.
#include "readyup/plugin_state.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

using namespace readyup::plugins;

static int g_failures = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

int main() {
  bool ok = false;
  CHECK(ParseDisabled("", &ok).empty() && ok);
  auto d = ParseDisabled(R"({"version": 1, "disabled": ["skins", "hello", 3, ""], "other": true})", &ok);
  CHECK(ok && d == (std::set<std::string>{"hello", "skins"}));
  CHECK(ParseDisabled("{not json", &ok).empty() && !ok);
  CHECK(ParseDisabled("[]", &ok).empty() && !ok);
  CHECK(ParseDisabled(R"({"version": 1})", &ok).empty() && ok);

  // Round trip; names that could break the JSON are never written.
  const std::string j = DisabledJson({"skins", "midas", "bad\"name"});
  CHECK(ParseDisabled(j, &ok) == (std::set<std::string>{"midas", "skins"}) && ok);
  CHECK(DisabledJson({}) == "{\n  \"version\": 1,\n  \"disabled\": []\n}\n");

  CHECK(PluginStatePath("/x/csgo/readyup/plugins") == "/x/csgo/readyup/plugins/plugins.json");
  CHECK(PluginStatePath("").empty());

  char tmpl[] = "/tmp/ru-plugin-state.XXXXXX";
  const char* dir = mkdtemp(tmpl);
  CHECK(dir != nullptr);
  if (dir) {
    const std::string path = std::string(dir) + "/plugins.json";
    CHECK(LoadDisabled(path).empty());  // missing file
    CHECK(SaveDisabled(path, {"skins"}));
    CHECK(LoadDisabled(path) == std::set<std::string>{"skins"});
    CHECK(SaveDisabled(path, {}));
    CHECK(LoadDisabled(path).empty());
    unlink(path.c_str());
    rmdir(dir);
  }
  std::printf("plugin_state_test: %s\n", g_failures ? "FAIL" : "PASS");
  return g_failures ? 1 : 0;
}
