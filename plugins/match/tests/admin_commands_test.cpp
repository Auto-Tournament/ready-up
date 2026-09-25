// Offline tests for readyup/admin_commands.h: `.ru map` / `.ru load` argument parsing, the help
// table, and the entry `ru reloadmap` loads (map_names.h ReloadEntry). ctest `match_admin_commands`.
// Permissions (admin-only, the console always) are checked in match_host_test.
#include "readyup/admin_commands.h"
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

static void TestMap() {
  std::string entry, err;
  CHECK(ParseMapCommand({"de_dust2"}, &entry, &err) && entry == "de_dust2");
  CHECK(ParseMapCommand({"3084291314"}, &entry, &err) && entry == "3084291314");
  CHECK(mapnames::LoadCommand(entry) == "host_workshop_map 3084291314");
  CHECK(ParseMapCommand({"ws:3084291314"}, &entry, &err) && entry == "ws:3084291314");
  CHECK(ParseMapCommand({"workshop/3084291314/aim_map"}, &entry, &err));
  entry = "unchanged";
  CHECK(!ParseMapCommand(Args{}, &entry, &err) && err.find("usage") != std::string::npos && entry == "unchanged");
  CHECK(!ParseMapCommand({"de_dust2", "extra"}, &entry, &err) && err.find("usage") != std::string::npos);
  CHECK(!ParseMapCommand({"de_dust2;quit"}, &entry, &err) && err.find("not a map name") != std::string::npos);
  CHECK(!ParseMapCommand({"../../etc"}, &entry, &err));
  CHECK(!ParseMapCommand({"123456789012345678901"}, &entry, &err));  // 21 digits
  CHECK(!ParseMapCommand({"de dust2"}, &entry, &err));
}

static void TestLoad() {
  std::string url, err;
  CHECK(ParseLoadCommand({"https://example.invalid/m.json"}, &url, &err) && url == "https://example.invalid/m.json");
  CHECK(ParseLoadCommand({"http://127.0.0.1:8080/m.json"}, &url, &err));
  CHECK(!ParseLoadCommand(Args{}, &url, &err) && err.find("usage") != std::string::npos);
  CHECK(!ParseLoadCommand({"file:///etc/passwd"}, &url, &err));
  CHECK(!ParseLoadCommand({"a", "b"}, &url, &err));
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

static void TestHelp() {
  const std::string line = AdminCommandsChatLine();
  for (const char* c : {".ru map", ".ru reloadmap", ".ru restart", ".ru load", ".ru end", ".ru match restart"}) {
    CHECK(line.find(c) != std::string::npos);
  }
  for (const auto& h : AdminCommandsHelp()) CHECK(h.usage && h.help && *h.help);
}

int main() {
  TestMap();
  TestLoad();
  TestReloadEntry();
  TestHelp();
  std::printf("admin_commands_test: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
