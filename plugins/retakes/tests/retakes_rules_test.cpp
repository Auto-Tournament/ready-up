// Offline tests for plugins/retakes/retakes_rules.h. ctest `retakes_rules`.
#include "retakes_rules.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>

using namespace retakes;

static int g_failures = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

// No shuffling: the order stays as given, so the results are exact.
static const Shuffle kKeep = [](std::vector<size_t>&) {};
// Reverses: a stand-in for a real shuffle that changes the order.
static const Shuffle kReverse = [](std::vector<size_t>& v) { std::reverse(v.begin(), v.end()); };

static void Put(std::string& b, const std::string& s) { b += s, b.push_back('\0'); }
static void Le(std::string& b, uint32_t v, int bytes) {
  for (int i = 0; i < bytes; ++i) b.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

static void TestVpk() {
  std::string header;
  Le(header, 0x55aa1234u, 4);
  Le(header, 2, 4);
  Le(header, 77, 4);
  header.append(16, '\0');
  CHECK(VpkTreeSize(reinterpret_cast<const uint8_t*>(header.data()), header.size()) == 77);
  std::string v1 = header;
  v1[4] = 1;
  CHECK(VpkTreeSize(reinterpret_cast<const uint8_t*>(v1.data()), v1.size()) == 0);

  // txt > maps > {de_dust2_retake, de_mirage_retake}; vtex_c > " " > logo (in the dir file).
  std::string tree;
  Put(tree, "txt");
  Put(tree, "maps");
  Put(tree, "de_dust2_retake");
  Le(tree, 0xdeadbeef, 4), Le(tree, 0, 2), Le(tree, 3, 2), Le(tree, 10, 4), Le(tree, 20, 4), Le(tree, 0xffff, 2);
  Put(tree, "de_mirage_retake");
  Le(tree, 0, 4), Le(tree, 2, 2), Le(tree, 1, 2), Le(tree, 100, 4), Le(tree, 50, 4), Le(tree, 0xffff, 2);
  tree += "//";
  Put(tree, "");
  Put(tree, "");
  Put(tree, "vtex_c");
  Put(tree, " ");
  Put(tree, "logo");
  Le(tree, 0, 4), Le(tree, 0, 2), Le(tree, kVpkInDir, 2), Le(tree, 7, 4), Le(tree, 9, 4), Le(tree, 0xffff, 2);
  Put(tree, "");
  Put(tree, "");
  Put(tree, "");
  const auto* t = reinterpret_cast<const uint8_t*>(tree.data());
  VpkEntry e;
  CHECK(VpkFind(t, tree.size(), "maps/de_mirage_retake.txt", &e) && e.archive == 1 && e.offset == 100 &&
        e.length == 50 && e.preload == "//");
  CHECK(VpkFind(t, tree.size(), "MAPS/De_Dust2_Retake.TXT", &e) && e.archive == 3 && e.offset == 10);
  CHECK(VpkFind(t, tree.size(), "logo.vtex_c", &e) && e.archive == kVpkInDir);
  CHECK(!VpkFind(t, tree.size(), "maps/de_nuke_retake.txt", &e));
  CHECK(!VpkFind(t, tree.size() - 20, "logo.vtex_c", &e));  // cut short: not found, no overread
}

static const char* kMirage = R"(// Spawn locations and angles.
"Retake"
{
	"Spawns"
	{
		"CT"
		{
			// A site
			"0"
			{
				"0" "-1677.126709 -1634.004761 -199.909851 -1.040606 -70.527283 0.000000" // ct spawn towards stairs
				"1" "-1656.067993 -1312.397339 -198.956573 -9.220211 46.770073 0.000000"
			}
			// B site
			"1"
			{
				"0" "39.600376 -674.276245 -123.007751 -2.541049 156.639526 0.000000"	// top of mid
			}
		}
		"T"
		{
			"0" { "0" "-138.005844 -1797.033203 -103.906189 4.114976 171.140121 0.000000" }
			"1" { "0" "-2071.244141 76.052170 -95.906189 2.034678 -80.853889 0.000000" "1" "1 2 3 4 5 6" }
		}
	}
})";

static void TestParse() {
  MapSpawns m;
  std::string err;
  CHECK(ParseRetakeFile(kMirage, &m, &err));
  CHECK(m.site[0].ct.size() == 2 && m.site[1].ct.size() == 1 && m.site[0].t.size() == 1 && m.site[1].t.size() == 2);
  CHECK(m.site[0].ct[0].pos[0] < -1677.0f && m.site[0].ct[0].pos[0] > -1677.2f && m.site[0].ct[0].ang[1] < -70.5f);
  CHECK(m.site[1].t[1].pos[2] == 3.0f && m.site[1].t[1].ang[2] == 6.0f);
  CHECK(m.Usable());
  CHECK(!ParseRetakeFile("\"Retake\" { \"Spawns\" { \"CT\" { \"0\" { \"0\" \"1 2 x\" } } } }", &m, &err) &&
        err.find("bad spawn") != std::string::npos);
  CHECK(!ParseRetakeFile("\"Other\" { }", &m, &err));
  CHECK(!ParseRetakeFile("\"Retake\" { \"Spawns\" {", &m, &err));
  MapSpawns half;
  CHECK(ParseRetakeFile("\"Retake\" { \"Spawns\" { \"CT\" { \"0\" { \"0\" \"1 2 3 0 0 0\" } } } }", &half, &err));
  CHECK(!half.Usable());
}

static void TestTargets() {
  const int want[] = {0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 5};
  for (int n = 0; n <= 12; ++n) CHECK(TargetTerrorists(n) == want[n]);
  CHECK(TargetTerrorists(20) == 10);
}

static Player P(uint64_t id, int team, int score = 0) {
  Player p;
  p.id = id, p.team = team, p.score = score;
  return p;
}
static Player Q(uint64_t id, int order) {
  Player p;
  p.id = id, p.queued = true, p.order = order;
  return p;
}
static bool Has(const std::vector<uint64_t>& v, uint64_t id) { return std::find(v.begin(), v.end(), id) != v.end(); }

static void TestTeams() {
  // First round: 9 joiners split 4 T / 5 CT; 2 more wait.
  std::vector<Player> ps;
  for (uint64_t i = 1; i <= 11; ++i) ps.push_back(Q(i, static_cast<int>(i)));
  Streak st;
  Teams t = NextTeams(ps, 0, 9, 5, &st, kKeep);
  CHECK(t.t.size() == 4 && t.ct.size() == 5 && t.queued.size() == 2);
  CHECK(Has(t.queued, 10) && Has(t.queued, 11));

  // Ts win: same teams, the streak grows; at 5 in a row everyone is shuffled.
  std::vector<Player> round = {P(1, kT), P(2, kT), P(3, kCT), P(4, kCT), P(5, kCT)};
  st.tWins = 3;
  t = NextTeams(round, kT, 9, 5, &st, kKeep);
  CHECK(st.tWins == 4 && !t.scrambled && Has(t.t, 1) && Has(t.t, 2) && t.t.size() == 2);
  t = NextTeams(round, kT, 9, 5, &st, kReverse);
  CHECK(st.tWins == 0 && t.scrambled && t.t.size() == 2 && t.ct.size() == 3);
  CHECK(Has(t.t, 5) && Has(t.t, 4));  // reversed order: the last two become T
  Streak never;
  never.tWins = 50;
  t = NextTeams(round, kT, 9, 0, &never, kReverse);
  CHECK(!t.scrambled && Has(t.t, 1));

  // CTs win: the best CT (score > 0) attacks next; the losing Ts defend.
  std::vector<Player> ctWin = {P(1, kT, 50), P(2, kT), P(3, kCT, 100), P(4, kCT), P(5, kCT, 25)};
  st.tWins = 4;
  t = NextTeams(ctWin, kCT, 9, 5, &st, kKeep);
  CHECK(st.tWins == 0 && t.t.size() == 2 && t.t[0] == 3 && t.t[1] == 5);
  CHECK(Has(t.ct, 1) && Has(t.ct, 2) && Has(t.ct, 4));

  // A player leaves: counts are evened out (4 players: 2 T).
  std::vector<Player> uneven = {P(1, kT), P(2, kT), P(3, kT), P(4, kCT)};
  t = NextTeams(uneven, kT, 9, 0, &st, kKeep);
  CHECK(t.t.size() == 2 && t.ct.size() == 2 && Has(t.ct, 3));
  // A newcomer fills the short side.
  std::vector<Player> join = {P(1, kT), P(2, kCT), P(3, kCT), Q(9, 1)};
  t = NextTeams(join, kT, 9, 0, &st, kKeep);
  CHECK(t.t.size() == 2 && Has(t.t, 9));
  // max_players lowered: the newest active player waits again.
  std::vector<Player> over = {P(1, kT), P(2, kCT), P(3, kCT)};
  t = NextTeams(over, 0, 2, 0, &st, kKeep);
  CHECK(t.t.size() + t.ct.size() == 2 && Has(t.queued, 3));
}

static void TestPlace() {
  SiteSpawns s;
  s.t.resize(4);
  s.ct.resize(3);
  // Spawns 2 and 3 are inside the bombsite.
  RoundSpawns r = PlaceRound(s, {11, 12, 13}, {21, 22, 23, 24}, {3}, kKeep);
  CHECK(r.planter == 11 && r.t[0].spawn == 3);
  std::set<int> used;
  for (const auto& p : r.t) used.insert(p.spawn);
  CHECK(used.size() == 3 && !used.count(-1));
  CHECK(r.ct.size() == 4 && r.ct[3].spawn == -1);  // 4 CTs, 3 spawns: the last has none
  // No spawn inside the site: any T spawn.
  r = PlaceRound(s, {11}, {}, {}, kKeep);
  CHECK(r.planter == 11 && r.t[0].spawn == 0);
  // Bad plantable indices are ignored.
  r = PlaceRound(s, {11}, {}, {9, -1}, kKeep);
  CHECK(r.t[0].spawn == 0);
  // No Ts: no planter.
  r = PlaceRound(s, {}, {21}, {3}, kKeep);
  CHECK(r.planter == 0 && r.t.empty());
}

int main() {
  TestVpk();
  TestParse();
  TestTargets();
  TestTeams();
  TestPlace();
  if (g_failures) std::fprintf(stderr, "%d failure(s)\n", g_failures);
  else std::printf("retakes_rules: ok\n");
  return g_failures ? 1 : 0;
}
