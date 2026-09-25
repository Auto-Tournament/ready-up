// Offline tests for readyup/ru_commands.h: main commands + subcommands and their help, which
// subcommands are admin-only, `.ru map change` / `.ru match load` parsing, and the entry
// `ru map reload` loads (map_names.h ReloadEntry). ctest `match_ru_commands`.
// Permissions end to end (non-admin refused, console and admins run) are in match_host_test.
#include "readyup/ru_commands.h"
#include "readyup/map_names.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace readyup;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    ++g_checks;                                                                     \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

using Args = std::vector<std::string>;

static bool Admin(const char* main, const char* sub) {
  const RuMainCommand* m = FindRuMain(main);
  const RuSubcommand* s = m ? FindRuSub(*m, sub) : nullptr;
  return s && s->admin;
}

static void TestTree() {
  std::vector<std::string> mains;
  for (const auto& m : MatchRuCommands()) mains.push_back(m.name);
  CHECK((mains == std::vector<std::string>{"match", "mode", "hud"}));
  // The old flat commands are gone: nothing named like them at the top level.
  for (const char* old : {"restart", "start", "end", "pause", "idle", "state", "fp", "reloadmap", "load", "side"}) {
    CHECK(FindRuMain(old) == nullptr);
  }
  CHECK(FindRuMain("map") == nullptr && FindRuMain("admins") == nullptr);  // the essentials plugin's
  // Admin-only vs public.
  for (const auto& p : std::vector<std::pair<const char*, const char*>>{
           {"match", "load"}, {"match", "start"}, {"match", "restart"}, {"match", "end"}, {"match", "pause"},
           {"match", "unpause"}, {"mode", "idle"}, {"mode", "practice"}, {"mode", "scrim"}, {"hud", "test"}}) {
    CHECK(Admin(p.first, p.second));
  }
  CHECK(!Admin("match", "state") && !Admin("match", "rules") && !Admin("match", "side") && !Admin("mode", "show"));
}

static void TestHelp() {
  const auto lines = RuHelpLines(*FindRuMain("mode"));
  CHECK(lines.size() == FindRuMain("mode")->subs.size() + 1);
  CHECK(lines[0] == ".ru mode: server mode");
  CHECK(RuHelpLines(*FindRuMain("match")).size() == FindRuMain("match")->subs.size() + 1);
  CHECK(RuUnknownSubReply("mode", "nope") == "Ready Up: unknown command \".ru mode nope\". Type .ru help mode for the list.");
}

static void TestMapChange() {
  std::string entry, err;
  CHECK(ParseMapChange({"de_dust2"}, &entry, &err) && entry == "de_dust2");
  CHECK(ParseMapChange({"3084291314"}, &entry, &err) && mapnames::LoadCommand(entry) == "host_workshop_map 3084291314");
  CHECK(ParseMapChange({"ws:3084291314"}, &entry, &err) && entry == "ws:3084291314");
  CHECK(ParseMapChange({"workshop/3084291314/aim_map"}, &entry, &err));
  entry = "unchanged";
  CHECK(!ParseMapChange(Args{}, &entry, &err) && err.find("usage") != std::string::npos && entry == "unchanged");
  CHECK(!ParseMapChange({"de_dust2", "extra"}, &entry, &err) && err.find("usage") != std::string::npos);
  CHECK(!ParseMapChange({"de_dust2;quit"}, &entry, &err) && err.find("not a map name") != std::string::npos);
  CHECK(!ParseMapChange({"../../etc"}, &entry, &err));
  CHECK(!ParseMapChange({"123456789012345678901"}, &entry, &err));  // 21 digits
}

static void TestMatchLoad() {
  std::string url, err;
  CHECK(ParseMatchLoad({"https://example.invalid/m.json"}, &url, &err) && url == "https://example.invalid/m.json");
  CHECK(ParseMatchLoad({"http://127.0.0.1:8080/m.json"}, &url, &err));
  CHECK(!ParseMatchLoad(Args{}, &url, &err) && err.find("usage") != std::string::npos);
  CHECK(!ParseMatchLoad({"file:///etc/passwd"}, &url, &err));
  CHECK(!ParseMatchLoad({"a", "b"}, &url, &err));
}

static void TestReloadEntry() {
  mapnames::ResetBindings();
  CHECK(mapnames::ReloadEntry("de_dust2") == "de_dust2");
  CHECK(mapnames::ReloadEntry("maps/de_nuke.vpk") == "de_nuke");
  CHECK(mapnames::ReloadEntry("workshop/3084291314/aim_map") == "workshop/3084291314/aim_map");
  // The engine reports a workshop map by its bsp name: the id bound at load time is used.
  mapnames::NoteWorkshopLoad("3070284539");
  mapnames::NoteMapLoaded("aim_botz");
  CHECK(mapnames::ReloadEntry("aim_botz") == "workshop/3070284539/aim_botz");
  CHECK(mapnames::LoadCommand(mapnames::ReloadEntry("aim_botz")) == "host_workshop_map 3070284539");
  CHECK(mapnames::ReloadEntry("") == "");
  CHECK(mapnames::ReloadEntry("de_x;quit") == "");
  mapnames::ResetBindings();
}

int main() {
  TestTree();
  TestHelp();
  TestMapChange();
  TestMatchLoad();
  TestReloadEntry();
  std::printf("ru_commands_test: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
