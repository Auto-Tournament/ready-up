// Offline tests for plugins/deathmatch/dm_rules.h. ctest `deathmatch_rules`.
#include "dm_rules.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>

using namespace deathmatch;

static int g_failures = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

static bool Has(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}
static bool Contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

static Settings FromMap(const std::map<std::string, std::string>& kv, std::vector<std::string>* warnings = nullptr) {
  return LoadSettings(
      [&](const char* key, std::string* out) {
        const auto it = kv.find(key);
        if (it == kv.end()) return false;
        *out = it->second;
        return true;
      },
      warnings);
}

constexpr uint64_t A = 76561198000000001ull, B = 76561198000000002ull, C = 76561198000000003ull;

static void TestModesAndSettings() {
  Mode m = Mode::Off;
  CHECK(ParseMode("ffa", &m) && m == Mode::Ffa);
  CHECK(ParseMode("TDM", &m) && m == Mode::Tdm);
  CHECK(ParseMode("dm", &m) && m == Mode::Ffa);
  CHECK(!ParseMode("off", &m) && !ParseMode("", &m));
  CHECK(std::string(ModeName(Mode::Ffa)) == "ffa" && std::string(ModeName(Mode::Tdm)) == "tdm");

  const Settings d = FromMap({});
  CHECK(d.killLimitFfa == 30 && d.killLimitTdm == 100 && d.timeLimitMinutes == 10 && d.spawnProtectionSeconds == 2);
  CHECK(!d.headshotOnly && d.hud && d.hudIntervalMs == 1000 && d.endDelaySeconds == 10 && !d.restartReloadsMap);
  CHECK(d.weaponRounds.empty() && d.weaponRoundEveryMinutes == 0 && d.restoreGameType == 0 && d.restoreGameMode == 1);

  std::vector<std::string> warnings;
  const Settings s = FromMap({{"kill_limit_ffa", "25"},
                              {"kill_limit_tdm", " 150 "},
                              {"time_limit_minutes", "0"},
                              {"spawn_protection_seconds", "99"},  // out of range: default
                              {"headshot_only", "yes"},
                              {"hud", "0"},
                              {"hud_interval_ms", "250"},
                              {"restart", "reload"},
                              {"weapon_rounds", "weapon_deagle, awp ,,bad;weapon, weapon_SSG08"},
                              {"weapon_round_every_minutes", "3"},
                              {"weapon_round_seconds", "45"},
                              {"weapon_round_restrict_buy", "1"},
                              {"end_delay_seconds", "abc"}},
                             &warnings);
  CHECK(s.killLimitFfa == 25 && s.killLimitTdm == 150 && s.timeLimitMinutes == 0);
  CHECK(s.spawnProtectionSeconds == 2 && s.endDelaySeconds == 10);
  CHECK(s.headshotOnly && !s.hud && s.hudIntervalMs == 250 && s.restartReloadsMap);
  CHECK(s.weaponRounds.size() == 3 && s.weaponRounds[0] == "weapon_deagle" && s.weaponRounds[1] == "weapon_awp" &&
        s.weaponRounds[2] == "weapon_ssg08");
  CHECK(s.weaponRoundEveryMinutes == 3 && s.weaponRoundSeconds == 45 && s.weaponRoundRestrictBuy);
  CHECK(warnings.size() == 3);  // spawn_protection_seconds, weapon_rounds "bad;weapon", end_delay_seconds
  CHECK(KillLimit(s, Mode::Ffa) == 25 && KillLimit(s, Mode::Tdm) == 150 && KillLimit(s, Mode::Off) == 0);
}

static void TestCvars() {
  Settings s;
  s.spawnProtectionSeconds = 3;
  s.headshotOnly = true;
  const auto ffa = ModeCommands(Mode::Ffa, s);
  CHECK(Has(ffa, "mp_teammates_are_enemies 1") && Has(ffa, "mp_dm_teammode 0"));
  CHECK(Has(ffa, "mp_respawn_immunitytime 3") && Has(ffa, "mp_damage_headshot_only 1"));
  CHECK(Has(ffa, "mp_ignore_round_win_conditions 1") && Has(ffa, "mp_timelimit 0") && Has(ffa, "mp_roundtime 60"));
  const auto tdm = ModeCommands(Mode::Tdm, Settings{});
  CHECK(Has(tdm, "mp_teammates_are_enemies 0") && Has(tdm, "mp_dm_teammode 1") && Has(tdm, "mp_damage_headshot_only 0"));
  CHECK(GameModeCommands() == std::vector<std::string>({"game_type 1", "game_mode 2"}));

  const auto leave = LeaveCommands(Settings{}, true);
  CHECK(Has(leave, "mp_teammates_are_enemies 0") && Has(leave, "mp_damage_headshot_only 0") &&
        Has(leave, "mp_ignore_round_win_conditions 0") && Has(leave, "mp_buy_allow_guns 255"));
  CHECK(Has(leave, "game_type 0") && Has(leave, "game_mode 1"));
  Settings wingman;
  wingman.restoreGameMode = 2;
  CHECK(Has(LeaveCommands(wingman, true), "game_mode 2"));
  CHECK(!Has(LeaveCommands(Settings{}, false), "game_type 0"));

  CHECK(ValidWeapon("weapon_ak47") && ValidWeapon("weapon_m4a1_silencer"));
  CHECK(!ValidWeapon("weapon_") && !ValidWeapon("ak47") && !ValidWeapon("weapon_ak47;quit") && !ValidWeapon("weapon_AK47"));
  CHECK(IsPistol("weapon_deagle") && !IsPistol("weapon_awp"));
  const auto deagle = WeaponRoundCommands("weapon_deagle", false);
  CHECK(Has(deagle, "mp_ct_default_secondary weapon_deagle") && Has(deagle, "mp_t_default_secondary weapon_deagle") &&
        Has(deagle, "mp_ct_default_primary \"\"") && Has(deagle, "mp_buy_allow_guns 255"));
  const auto awp = WeaponRoundCommands("weapon_awp", true);
  CHECK(Has(awp, "mp_ct_default_primary weapon_awp") && Has(awp, "mp_t_default_primary weapon_awp") &&
        Has(awp, "mp_buy_allow_guns 0"));
  const auto back = WeaponRoundCommands("", true);
  CHECK(Has(back, "mp_t_default_secondary weapon_glock") && Has(back, "mp_buy_allow_guns 255"));
  CHECK(WeaponRoundCommands("weapon_x;quit", false) == back);
  CHECK(WeaponLabel("weapon_deagle") == "Desert Eagle" && WeaponLabel("weapon_mag7") == "mag7");
}

static void TestWeaponRounds() {
  Settings s;
  CHECK(WeaponRoundAt(s, 1000).index == -1);  // off
  s.weaponRounds = {"weapon_deagle", "weapon_awp"};
  s.weaponRoundEveryMinutes = 2;
  s.weaponRoundSeconds = 30;
  CHECK(WeaponRoundAt(s, 0).index == -1 && WeaponRoundAt(s, 119).index == -1);
  WeaponRound w = WeaponRoundAt(s, 120);
  CHECK(w.index == 0 && w.weapon == "weapon_deagle" && w.secondsLeft == 30);
  CHECK(WeaponRoundAt(s, 149.5).index == 0 && WeaponRoundAt(s, 150).index == -1);
  w = WeaponRoundAt(s, 245);
  CHECK(w.index == 1 && w.weapon == "weapon_awp" && w.secondsLeft == 25);
  CHECK(WeaponRoundAt(s, 360).index == 0);  // wraps around
  s.weaponRoundSeconds = 999;  // capped at the period
  CHECK(WeaponRoundAt(s, 239).index == 0 && WeaponRoundAt(s, 240).index == 1);
}

static void TestScoring() {
  Scoreboard b;
  b.SeePlayer(A, "alice", 3);
  b.SeePlayer(B, "bob", 2);
  b.SeePlayer(C, "carol", 3);
  // FFA: teammates count, suicide / world only add a death.
  CHECK(b.OnKill(Mode::Ffa, A, 3, C, 3, true, 1.0));
  CHECK(b.OnKill(Mode::Ffa, A, 3, B, 2, false, 2.0));
  CHECK(!b.OnKill(Mode::Ffa, B, 2, B, 2, false, 3.0));  // suicide
  CHECK(!b.OnKill(Mode::Ffa, 0, 0, C, 3, false, 4.0));  // world
  CHECK(!b.OnKill(Mode::Off, A, 3, B, 2, false, 5.0));
  const PlayerScore* a = b.Find(A);
  CHECK(a && a->kills == 2 && a->headshots == 1 && a->deaths == 0);
  CHECK(b.Find(B)->deaths == 2 && b.Find(C)->deaths == 2 && b.Find(B)->kills == 0);
  CHECK(b.TeamKills(3) == 0);  // FFA keeps no team score

  // TDM: team kills and kills without a side do not count.
  Scoreboard t;
  CHECK(t.OnKill(Mode::Tdm, A, 3, B, 2, false, 1.0));
  CHECK(!t.OnKill(Mode::Tdm, A, 3, C, 3, false, 2.0));  // team kill
  CHECK(!t.OnKill(Mode::Tdm, A, 0, B, 2, false, 3.0));
  CHECK(t.OnKill(Mode::Tdm, B, 2, A, 3, false, 4.0));
  CHECK(t.OnKill(Mode::Tdm, B, 2, C, 3, false, 5.0));
  CHECK(t.TeamKills(3) == 1 && t.TeamKills(2) == 2 && t.Find(A)->kills == 1 && t.Find(C)->deaths == 2);

  // Ordering: kills desc, deaths asc, who got there first, name.
  Scoreboard r;
  r.SeePlayer(A, "zed", 2);
  r.SeePlayer(B, "amy", 2);
  r.SeePlayer(C, "bo", 2);
  const uint64_t D = 76561198000000004ull, E = 76561198000000005ull;
  r.SeePlayer(D, "dee", 3);
  r.SeePlayer(E, "eve", 3);
  r.OnKill(Mode::Ffa, A, 2, D, 3, false, 10);  // zed 1 @10
  r.OnKill(Mode::Ffa, B, 2, E, 3, false, 5);   // amy 1 @5
  r.OnKill(Mode::Ffa, C, 2, D, 3, false, 20);  // bo 1 @20
  r.OnKill(Mode::Ffa, C, 2, E, 3, false, 30);  // bo 2
  r.OnKill(Mode::Ffa, D, 3, A, 2, false, 40);  // dee 1, zed 1 death
  auto ranked = r.Ranked();
  CHECK(ranked.size() == 5);
  CHECK(ranked[0].id == C);  // bo: 2 kills
  CHECK(r.RankOf(C) == 1 && r.RankOf(76561198000000099ull) == 0);
  // amy (1k 0d), zed (1k 1d), dee (1k 2d): fewer deaths first; eve (0k) last.
  CHECK(ranked[1].name == "amy" && ranked[2].name == "zed" && ranked[3].name == "dee" && ranked[4].name == "eve");
  // Same kills and deaths: who got there first.
  Scoreboard tie;
  tie.OnKill(Mode::Ffa, A, 2, D, 3, false, 9);
  tie.OnKill(Mode::Ffa, B, 2, E, 3, false, 4);
  CHECK(tie.RankOf(B) == 1 && tie.RankOf(A) == 2);

  // Bots get pseudo ids, never a SteamID.
  CHECK(BotId(3) != BotId(4) && (BotId(3) >> 60) == 0xB && BotId(3) != 3);

  // Stash round trip.
  Scoreboard copy;
  CHECK(copy.Deserialize(t.Serialize()));
  CHECK(copy.TeamKills(2) == 2 && copy.TeamKills(3) == 1 && copy.Find(B)->kills == 2 && copy.Find(C)->deaths == 2);
  Scoreboard named;
  named.SeePlayer(A, "tab\there", 2);
  CHECK(copy.Deserialize(named.Serialize()) && copy.Find(A)->name == "tab here");
  CHECK(!copy.Deserialize("garbage\n") && copy.Find(A));  // a bad blob keeps the board
}

static void TestWinConditions() {
  Settings s;
  s.killLimitFfa = 3;
  s.killLimitTdm = 2;
  s.timeLimitMinutes = 5;
  Scoreboard b;
  b.SeePlayer(A, "alice", 3);
  b.SeePlayer(B, "bob", 2);
  CHECK(!CheckEnd(Mode::Ffa, b, s, 10).over);
  b.OnKill(Mode::Ffa, A, 3, B, 2, false, 1);
  b.OnKill(Mode::Ffa, A, 3, B, 2, false, 2);
  CHECK(!CheckEnd(Mode::Ffa, b, s, 10).over);
  b.OnKill(Mode::Ffa, A, 3, B, 2, false, 3);
  Outcome o = CheckEnd(Mode::Ffa, b, s, 10);
  CHECK(o.over && !o.draw && o.reason == "kill_limit" && o.winnerId == A && o.winnerName == "alice" && o.winnerKills == 3);
  CHECK(Contains(WinnerChat(Mode::Ffa, o), "alice wins with 3 kills (kill limit)"));

  // Time limit: the leader wins; equal kills at the top or nobody scoring = draw.
  Scoreboard t;
  t.OnKill(Mode::Ffa, A, 3, B, 2, false, 1);
  t.SeePlayer(A, "alice", 3);
  CHECK(!CheckEnd(Mode::Ffa, t, s, 299).over);
  o = CheckEnd(Mode::Ffa, t, s, 300);
  CHECK(o.over && o.reason == "time_limit" && o.winnerId == A);
  t.OnKill(Mode::Ffa, B, 2, A, 3, false, 2);
  o = CheckEnd(Mode::Ffa, t, s, 300);
  CHECK(o.over && o.draw && o.winnerKills == 1);
  CHECK(Contains(WinnerChat(Mode::Ffa, o), "draw at 1 kills"));
  CHECK(CheckEnd(Mode::Ffa, Scoreboard{}, s, 301).draw);
  Settings noLimits;
  noLimits.killLimitFfa = 0;
  noLimits.timeLimitMinutes = 0;
  CHECK(!CheckEnd(Mode::Ffa, b, noLimits, 1e9).over);
  CHECK(!CheckEnd(Mode::Off, b, s, 1e9).over);

  // TDM: a team at the kill limit; at the time limit the team ahead, equal = draw.
  Scoreboard tdm;
  tdm.OnKill(Mode::Tdm, A, 3, B, 2, false, 1);
  CHECK(!CheckEnd(Mode::Tdm, tdm, s, 1).over);
  tdm.OnKill(Mode::Tdm, A, 3, B, 2, false, 2);
  o = CheckEnd(Mode::Tdm, tdm, s, 1);
  CHECK(o.over && o.winnerTeam == 3 && o.winnerName == "Counter-Terrorists" && o.winnerKills == 2 && o.runnerUpKills == 0);
  CHECK(Contains(WinnerChat(Mode::Tdm, o), "Counter-Terrorists win 2 : 0 (kill limit)"));
  Scoreboard even;
  even.OnKill(Mode::Tdm, A, 3, B, 2, false, 1);
  even.OnKill(Mode::Tdm, B, 2, A, 3, false, 2);
  o = CheckEnd(Mode::Tdm, even, s, 300);
  CHECK(o.over && o.draw && o.reason == "time_limit");
}

static void TestHtml() {
  CHECK(HtmlEscape("a<b>&\"c'", 50) == "a&#60;b&#62;&#38;&#34;c&#39;");
  CHECK(HtmlEscape("\x03red\x01 name", 50) == "red name");  // chat color bytes dropped
  CHECK(HtmlEscape("abcdefghij", 4) == "abcd...");
  CHECK(HtmlEscape("\xC3\xA6\xC3\xB8\xC3\xA5x", 3) == "\xC3\xA6\xC3\xB8\xC3\xA5...");  // counts characters, not bytes
  CHECK(HtmlEscape("<script>", 100).find('<') == std::string::npos);
  CHECK(FormatClock(425) == "7:05" && FormatClock(-3) == "0:00");

  Scoreboard b;
  const char* names[] = {"p1", "p2", "p3", "p4", "p5", "p6", "<p7>"};
  for (int i = 0; i < 7; ++i) b.SeePlayer(A + static_cast<uint64_t>(i), names[i], 2);
  // p1 has 7 kills, p2 6, ... <p7> 1.
  for (int i = 0; i < 7; ++i) {
    for (int k = 0; k < 7 - i; ++k) b.OnKill(Mode::Ffa, A + static_cast<uint64_t>(i), 2, C + 100, 3, false, k);
  }
  const uint64_t p7 = A + 6;
  std::string h = LeaderboardHtml(Mode::Ffa, b, p7, 30, 425, "");
  CHECK(Contains(h, "FFA") && Contains(h, "first to 30") && Contains(h, "7:05"));
  CHECK(Contains(h, "1. p1 &#183; 7") && Contains(h, "5. p5 &#183; 3"));
  CHECK(!Contains(h, "6. p6"));                     // top 5 only ...
  CHECK(Contains(h, "you: #7 &#183; 1"));           // ... plus your own rank
  CHECK(!Contains(h, "<p7>"));
  h = LeaderboardHtml(Mode::Ffa, b, A + 1, 0, -1, "weapon_awp");
  CHECK(Contains(h, "#A3E635'>2. p2"));  // your row highlighted
  CHECK(!Contains(h, "you: #") && !Contains(h, "first to") && Contains(h, "AWP round"));
  CHECK(h.size() < 1200);  // small

  Scoreboard t;
  t.SeePlayer(A, "alice", 3);
  t.OnKill(Mode::Tdm, A, 3, B, 2, false, 1);
  h = LeaderboardHtml(Mode::Tdm, t, B, 100, -1, "");
  CHECK(Contains(h, "TDM") && Contains(h, "CT 1") && Contains(h, "0 T"));
  CHECK(Contains(LeaderboardHtml(Mode::Ffa, Scoreboard{}, A, 30, -1, ""), "no kills yet"));

  Outcome o;
  o.over = true;
  o.reason = "kill_limit";
  o.winnerName = "<b>boss</b>";
  o.winnerKills = 30;
  const std::string w = WinnerHtml(Mode::Ffa, o, 7);
  CHECK(Contains(w, "&#60;b&#62;boss") && Contains(w, "WINS") && Contains(w, "30 kills") && Contains(w, "next game in 7 s"));
  o.draw = true;
  CHECK(Contains(WinnerHtml(Mode::Tdm, o, 0), "DRAW"));
}

static void TestMaps() {
  CHECK(MapArgToEntry("https://steamcommunity.com/sharedfiles/filedetails/?id=3084291314&searchtext=aim") == "3084291314");
  CHECK(MapArgToEntry("de_dust2") == "de_dust2");
  MapChoice c = ResolveMap("aim_map", "de_dust2", "de_mirage");
  CHECK(c.entry == "aim_map" && c.source == "argument");
  c = ResolveMap("https://steamcommunity.com/sharedfiles/filedetails/?id=123", "de_dust2", "de_mirage");
  CHECK(c.entry == "123" && c.source == "argument");
  c = ResolveMap("", "workshop/3084291314/aim_x", "de_mirage");
  CHECK(c.entry == "workshop/3084291314/aim_x" && c.source == "default");
  c = ResolveMap("", "", "de_mirage");
  CHECK(c.entry == "de_mirage" && c.source == "current");
  c = ResolveMap("", "", "");
  CHECK(c.entry.empty() && c.source == "current");
}

int main() {
  TestModesAndSettings();
  TestCvars();
  TestWeaponRounds();
  TestScoring();
  TestWinConditions();
  TestHtml();
  TestMaps();
  std::printf("deathmatch_rules_test: %s\n", g_failures ? "FAIL" : "PASS");
  return g_failures ? 1 : 0;
}
