// Offline tests for plugins/essentials/essentials_rules.h. ctest `essentials_rules`.
#include "essentials_rules.h"

#include <cstdio>

using namespace essentials;

static int g_failures = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

int main() {
  constexpr uint64_t A = 76561198000000001ull, B = 76561198000000002ull;
  bool ok = false;
  auto admins = ParseAdmins(R"({"version": 1, "admins": [{"steamid64": "76561198000000001", "name": "alice"},
      "76561198000000002", {"steamid64": "76561198000000001"}, {"steamid64": "nope"}]})", &ok);
  CHECK(ok && admins.size() == 2 && admins[0].name == "alice" && IsAdmin(admins, B));
  {
    const auto numeric = ParseAdmins(R"({"version": 1, "admins": [{"steamid64": 76561198000000009}]})", &ok);
    CHECK(ok && numeric.size() == 1 && numeric[0].steamid64 == 76561198000000009ull);
  }
  CHECK(ParseAdmins("", &ok).empty() && ok);
  CHECK(ParseAdmins("{bad", &ok).empty() && !ok);
  const auto back = ParseAdmins(AdminsJson(admins), &ok);
  CHECK(ok && back.size() == 2 && back[0].steamid64 == A && back[0].name == "alice");

  std::vector<Admin> v;
  CHECK(AddAdmin(&v, A, "alice") && !AddAdmin(&v, A, "again") && !AddAdmin(&v, 0, "x"));
  CHECK(IsAdmin(v, A) && !IsAdmin(v, B) && !IsAdmin(v, 0));
  CHECK(RemoveAdmin(&v, A) && !RemoveAdmin(&v, A) && v.empty());

  CHECK(ParseSteamId64("76561198000000001") == A && ParseSteamId64("123") == 0);

  const std::vector<Player> players = {{A, "Simpert"}, {B, "Simon"}, {3, "bob"}};
  Player p;
  std::string err;
  CHECK(FindPlayer(players, "simpert", &p, &err) && p.steamid64 == A);
  CHECK(FindPlayer(players, "BO", &p, &err) && p.name == "bob");
  CHECK(!FindPlayer(players, "sim", &p, &err) && err.find("2 players") != std::string::npos);
  CHECK(!FindPlayer(players, "zzz", &p, &err) && err.find("no connected") != std::string::npos);
  CHECK(!FindPlayer(players, "", &p, &err));

  CHECK(MapArgToEntry("https://steamcommunity.com/sharedfiles/filedetails/?id=3793104017") == "3793104017");
  CHECK(MapArgToEntry("steamcommunity.com/sharedfiles/filedetails/?id=3793104017&searchtext=aim") == "3793104017");
  CHECK(MapArgToEntry("https://steamcommunity.com/workshop/filedetails/?l=english&id=123456") == "123456");
  CHECK(MapArgToEntry("de_dust2") == "de_dust2" && MapArgToEntry("3793104017") == "3793104017");
  CHECK(MapArgToEntry("https://example.com/?id=5") == "https://example.com/?id=5");
  for (const char* m : {"match_live", "match_knife", "knife"}) CHECK(MapCommandBlocked(m));
  for (const char* m : {"", "idle", "practice", "scrim_warmup", "match_warmup", "postgame"}) CHECK(!MapCommandBlocked(m));
  {
    const std::string h = DownloadPanelHtml("de_<x>", 50, 100, 10);
    CHECK(h.find("de_&lt;x&gt;") != std::string::npos);
    CHECK(h.find("50.0%") != std::string::npos);
    CHECK(h.find("<font color='#4ade80'>\u2588\u2588\u2588\u2588\u2588</font>") != std::string::npos);
    CHECK(h.find("<font color='#3f3f46'>\u2588\u2588\u2588\u2588\u2588</font>") != std::string::npos);
    const std::string w = DownloadPanelHtml("m", 0, 0, 10);
    CHECK(w.find("waiting for Steam") != std::string::npos);
    CHECK(w.find("#4ade80") == std::string::npos);
    const std::string f = DownloadPanelHtml("m", 300, 200, 10);  // clamped
    CHECK(f.find("100.0%") != std::string::npos);
    CHECK(f.find("#3f3f46") == std::string::npos);
  }
  std::printf("essentials_rules_test: %s\n", g_failures ? "FAIL" : "PASS");
  return g_failures ? 1 : 0;
}
