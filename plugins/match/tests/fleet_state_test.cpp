// Offline tests for the engine-free half of the fleet match link (readyup/fleet_state.h) and the
// stats rewind used by round restores (match_stats.h RewindTo): merge patches and live_rev,
// epoch fencing, config_rev compare-and-set, match.assign -> MAT config (through the real
// parser), match.update ops, sha256 / base64, validators, workshop map entries / loaded map names
// (map_names.h) and failover resume blocks. ctest `match_fleet_state`.
#include "readyup/fleet_state.h"
#include "readyup/map_names.h"
#include "readyup/match_config_parser.h"
#include "readyup/match_stats.h"

#include <cstdio>
#include <string>

using namespace readyup;
using fleetstate::Json;
namespace fs = readyup::fleetstate;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                     \
  do {                                                                                  \
    ++g_checks;                                                                         \
    if (!(cond)) {                                                                      \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);     \
      ++g_failures;                                                                     \
    }                                                                                   \
  } while (0)

#define CHECK_STR(a, b)                                                                                   \
  do {                                                                                                    \
    ++g_checks;                                                                                           \
    const std::string va = (a), vb = (b);                                                                 \
    if (va != vb) {                                                                                       \
      std::fprintf(stderr, "%s:%d: CHECK_STR failed: %s\n  got:  %s\n  want: %s\n", __FILE__, __LINE__, #a, \
                   va.c_str(), vb.c_str());                                                               \
      ++g_failures;                                                                                       \
    }                                                                                                     \
  } while (0)

static Json J(const std::string& text) {
  Json v;
  std::string err;
  if (!Json::Parse(text, &v, &err)) {
    std::fprintf(stderr, "bad test JSON: %s: %s\n", err.c_str(), text.c_str());
    ++g_failures;
  }
  return v;
}

// ---------------------------------------------------------------------------- merge patch / rev

static void TestLiveStream() {
  fs::LiveStream s;
  const Json base = J(R"({"match_id":"m1","epoch":2,"live_rev":0,"phase":"warmup","side":null,
    "teams":{"team1":{"players":{"7656":{"ready":false,"connected":true}}}},"pause":{"active":false}})");
  s.Reset(base, 0);
  CHECK(s.Rev() == 0);
  CHECK(s.State().Find("side") == nullptr);  // nulls stripped from the baseline

  // No change: no rev.
  Json patch;
  long long rev = -1;
  CHECK(!s.Advance(base, false, &patch, &rev));
  CHECK(s.Rev() == 0);

  // One field deep in the roster: the patch has only that path + live_rev.
  Json next = base;
  next["teams"]["team1"]["players"]["7656"]["ready"] = true;
  CHECK(s.Advance(next, false, &patch, &rev));
  CHECK(rev == 1);
  CHECK_STR(patch.Dump(), R"({"live_rev":1,"teams":{"team1":{"players":{"7656":{"ready":true}}}}})");

  // Forced (an event without a state change): rev + 1, patch = live_rev only.
  CHECK(s.Advance(next, true, &patch, &rev));
  CHECK(rev == 2);
  CHECK_STR(patch.Dump(), R"({"live_rev":2})");

  // A removed member becomes null; applying every patch in order reproduces the state.
  Json replay = fs::StripNulls(base);
  replay["live_rev"] = 0;
  s.Reset(replay, 0);
  Json a = next;
  a["phase"] = "live";
  Json b = a;
  b["pause"] = J(R"({"active":true,"type":"admin","by":"platform:u1"})");
  Json c = b;
  c["pause"] = J(R"({"active":false})");
  c["round"] = J(R"({"number":3})");
  for (const Json* st : {&a, &b, &c}) {
    CHECK(s.Advance(*st, false, &patch, &rev));
    replay = status::MergePatchApply(replay, patch);
  }
  CHECK(rev == 3);
  Json want = fs::StripNulls(c);
  want["live_rev"] = 3;
  CHECK(replay == want);
  CHECK(replay == s.State());
  // pause.type / pause.by were removed with a null in the last patch.
  CHECK(replay.Find("pause")->Find("type") == nullptr);
}

// ---------------------------------------------------------------------------- epoch fence / CAS

static void TestFence() {
  fs::Fence f;
  fs::Assignment none;
  CHECK(f.CheckAssign(none, "m1", 1, false) == fs::Verdict::Ok);
  CHECK(f.CheckAssign(none, "m1", 0, false) == fs::Verdict::StaleEpoch);
  CHECK(f.CheckAssign(none, "m1", 1, /*localMatchActive=*/true) == fs::Verdict::Busy);

  fs::Assignment cur;
  cur.active = true;
  cur.match_id = "m1";
  cur.epoch = 3;
  f.Retire("m1", 3);
  CHECK(f.CheckAssign(cur, "m1", 3, false) == fs::Verdict::Duplicate);   // replayed assign
  CHECK(f.CheckAssign(cur, "m1", 2, false) == fs::Verdict::StaleEpoch);  // late, lower epoch
  CHECK(f.CheckAssign(cur, "m1", 4, false) == fs::Verdict::Ok);          // re-issued, newer epoch
  CHECK(f.CheckAssign(cur, "m2", 1, false) == fs::Verdict::Busy);        // one match at a time

  CHECK(f.CheckScoped(cur, "m1", 3) == fs::Verdict::Ok);
  CHECK(f.CheckScoped(cur, "m1", 2) == fs::Verdict::StaleEpoch);
  CHECK(f.CheckScoped(cur, "m1", 4) == fs::Verdict::NotAssigned);
  CHECK(f.CheckScoped(cur, "m2", 1) == fs::Verdict::NotAssigned);
  CHECK_STR(fs::VerdictCode(fs::Verdict::StaleEpoch), "stale_epoch");

  // After the unassign (retired at epoch 3): a late assign / cmd of that epoch is fenced off.
  CHECK(f.CheckAssign(none, "m1", 3, false) == fs::Verdict::StaleEpoch);
  CHECK(f.CheckAssign(none, "m1", 2, false) == fs::Verdict::StaleEpoch);
  CHECK(f.CheckScoped(none, "m1", 3) == fs::Verdict::StaleEpoch);
  CHECK(f.CheckAssign(none, "m1", 4, false) == fs::Verdict::Ok);

  // Survives a plugin reload.
  fs::Fence g;
  g.FromJson(f.ToJson());
  CHECK(g.Retired("m1") == 3);
  // Bounded.
  for (int i = 0; i < 100; ++i) g.Retire("x" + std::to_string(i), 1);
  CHECK(g.Retired("m1") == 0);
  CHECK(g.Retired("x99") == 1);

  CHECK(fs::ConfigCas(4, 4));
  CHECK(!fs::ConfigCas(3, 4));
}

// ---------------------------------------------------------------------------- assign / update

static const char* kAssign = R"({
  "match_id": "ko-r1-m3", "epoch": 1, "config_rev": 5,
  "config": {
    "num_maps": 3,
    "maps": [{"number":1,"name":"de_mirage","sides":"knife"},
             {"number":2,"name":"de_inferno","sides":"team1_ct"},
             {"number":3,"name":"de_nuke","workshop_id":"3070284539","sides":"team2_ct"}],
    "team1": {"id":"t-a","name":"Alpha","tag":"ALP","flag":"NO","captain":"76561198000000001",
              "players":[{"steamid64":"76561198000000001","name":"a1"},
                         {"steamid64":"76561198000000002","name":"a2","role":"sub"},
                         {"steamid64":"76561198000000003","name":"coachA","role":"coach"}]},
    "team2": {"id":"t-b","name":"Bravo","players":[{"steamid64":"76561198000000011","name":"b1"}]},
    "spectators": ["76561198000000099"],
    "admins": ["76561198000000077"],
    "password": "s3cret-pw",
    "rules": {"max_rounds": 24, "overtime": {"enabled": true, "rounds_per_half": 3, "max_overtimes": 2},
              "tiebreak": {"damage": true, "sudden_death_on_tie": false},
              "ready": {"min_per_team": 4, "allow_force_ready": false, "autoready": false},
              "pause": {"tactical_per_team": 2, "tactical_seconds": 45, "technical_per_team": 5,
                        "technical_seconds": 120, "unpause": "caller_team"},
              "forfeit": {"team_absent_seconds": 0},
              "knife": {"side_pick_seconds": 45}, "clinch_series": false, "whitelist": true},
    "cvars": {"mp_freezetime": 12, "sv_cheats": 1, "ru_hack": "1", "tv_delay": "90", "sv_password": "x",
              "mp_bad;cmd": "1", "mp_quote": "a\"b"}
  }
})";

static void TestAssign() {
  const Json p = J(kAssign);
  std::string err;
  CHECK(fs::ValidateAssign(p, &err));
  if (!err.empty()) std::fprintf(stderr, "validate: %s\n", err.c_str());

  std::vector<std::string> dropped;
  const Json mat = fs::AssignToMatConfig("ko-r1-m3", *p.Find("config"), &dropped);
  CHECK(dropped.size() == 5);  // sv_cheats, ru_hack, sv_password, mp_bad;cmd, mp_quote
  auto ctx = ParseWebhookMatchContextFromJson(mat.Dump(), &err);
  CHECK(ctx.has_value());
  if (!ctx) {
    std::fprintf(stderr, "parse: %s\n%s\n", err.c_str(), mat.Dump().c_str());
    return;
  }
  CHECK(ctx->matchid == fs::NumericMatchId("ko-r1-m3"));
  CHECK_STR(ctx->slug, "ko-r1-m3");
  CHECK(ctx->num_maps == 3);
  CHECK(ctx->maplist.size() == 3 && ctx->maplist[2] == "workshop/3070284539/de_nuke");  // host_workshop_map
  CHECK(ctx->map_sides.size() == 3 && ctx->map_sides[1] == "team1_ct");
  CHECK_STR(ctx->team1_name, "Alpha");
  CHECK(ctx->team1_captain_steamid64 == 76561198000000001ull);
  CHECK(ctx->roster_team.size() == 3);  // a1, a2 (sub), b1; the coach is a spectator
  CHECK(ctx->roster_team.count(76561198000000002ull) == 1);
  CHECK(ctx->roster_team.count(76561198000000003ull) == 0);
  CHECK(ctx->spectators.count(76561198000000003ull) == 1);
  CHECK(ctx->spectators.count(76561198000000099ull) == 1);
  CHECK(ctx->admins.count(76561198000000077ull) == 1);
  CHECK(ctx->maxRounds == 24);
  CHECK(ctx->overtime_enabled && ctx->overtimeSegments == 3 && ctx->maxOvertimes == 2);
  CHECK(ctx->damageTiebreakEnabled && !ctx->suddenDeathOnDamageTie);
  CHECK(ctx->knifeDecisionSeconds == 45);
  CHECK(!ctx->clinch_series);
  CHECK(ctx->cvars.count("mp_freezetime") && ctx->cvars["mp_freezetime"] == "12");
  CHECK(ctx->cvars["tv_delay"] == "90");
  CHECK(ctx->cvars["mp_maxrounds"] == "24");
  CHECK(ctx->cvars["mp_overtime_maxrounds"] == "6");
  CHECK(ctx->cvars.count("sv_cheats") == 0 && ctx->cvars.count("ru_hack") == 0 && ctx->cvars.count("sv_password") == 0);
  // Pause / ready / forfeit rules and team flags (match_rules.h); tactical timeouts are engine cvars.
  CHECK_STR(ctx->team1_flag, "NO");
  CHECK(ctx->team2_flag.empty());
  CHECK(ctx->rules.tech_pauses_per_team == 5);
  CHECK(ctx->rules.tech_pause_max_seconds == 120);
  CHECK(ctx->rules.both_teams_unpause == 0);
  CHECK(ctx->rules.allow_force_ready == 0);
  CHECK(ctx->rules.min_players_to_ready == 4);
  CHECK(ctx->rules.forfeit_after_seconds == 0);
  CHECK(ctx->cvars["mp_team_timeout_max"] == "2");
  CHECK(ctx->cvars["mp_team_timeout_time"] == "45");
  {
    // Rules left out stay unset (-1) so the server's readyup.cfg / defaults apply.
    Json bare = *p.Find("config");
    bare["rules"] = J(R"({"max_rounds": 24})");
    auto c2 = ParseWebhookMatchContextFromJson(fs::AssignToMatConfig("x", bare, nullptr).Dump(), &err);
    CHECK(c2 && c2->rules.tech_pauses_per_team == -1 && c2->rules.forfeit_after_seconds == -1 &&
          c2->rules.both_teams_unpause == -1 && c2->cvars.count("mp_team_timeout_max") == 0);
  }

  CHECK(fs::NumericMatchId("12345") == 12345);
  CHECK(fs::NumericMatchId("abc") != 0 && fs::NumericMatchId("abc") < (1ull << 52));
  CHECK(fs::NumericMatchId("abc") == fs::NumericMatchId("abc"));

  // Hand-over membership (D16).
  const Json& cfg = *p.Find("config");
  CHECK(fs::InAssignedMatch(cfg, 76561198000000001ull));
  CHECK(fs::InAssignedMatch(cfg, 76561198000000003ull));  // coach
  CHECK(fs::InAssignedMatch(cfg, 76561198000000099ull));  // spectator
  CHECK(fs::InAssignedMatch(cfg, 76561198000000077ull));  // match admin
  CHECK(!fs::InAssignedMatch(cfg, 76561198000000555ull));
  CHECK_STR(fs::TeamOf(cfg, 76561198000000011ull), "team2");
  CHECK_STR(fs::TeamOf(cfg, 76561198000000003ull), "spectator");

  // Invalid payloads.
  auto bad = [&](const char* path, const Json& value) {
    Json q = p;
    Json* cur = &q;
    std::string ps = path;
    size_t pos = 0;
    while ((pos = ps.find('.')) != std::string::npos) {
      cur = &(*cur)[ps.substr(0, pos)];
      ps = ps.substr(pos + 1);
    }
    (*cur)[ps] = value;
    std::string e;
    return !fs::ValidateAssign(q, &e) && !e.empty();
  };
  CHECK(bad("epoch", Json(0)));
  CHECK(bad("match_id", Json("bad id")));
  CHECK(bad("config.password", Json("has space")));
  CHECK(bad("config.password", Json("semi;colon")));
  CHECK(bad("config.num_maps", Json(5)));  // only 3 maps listed
  CHECK(bad("config.maps", J(R"([{"name":"de_dust2; quit","sides":"knife"}])")));
  CHECK(bad("config.team1", J(R"({"name":"X","players":[{"steamid64":"123abc"}]})")));
  CHECK(bad("config.team2", J(R"({"name":"","players":[]})")));
}

// Rulesets through match.assign: rules.ruleset / rules.overrides validated (cmd.result
// invalid_config), knife sides refused under valve, the MAT config carries them, coaches listed.
static void TestRulesetAssign() {
  const Json p = J(kAssign);
  std::string err;
  auto with = [&](const std::string& rulesPatch, const char* maps) {
    Json q = p;
    Json& c = q["config"];
    c["rules"] = status::MergePatchApply(*c.Find("rules"), J(rulesPatch));
    if (maps) c["maps"] = J(maps);
    return q;
  };
  const char* veto = R"([{"name":"de_mirage","sides":"team1_ct"},{"name":"de_inferno","sides":"team2_ct"},
                         {"name":"de_nuke","sides":"team1_ct"}])";
  // kAssign map 1 is a knife map: refused under valve, with the reason.
  CHECK(!fs::ValidateAssign(with(R"({"ruleset":"valve"})", nullptr), &err));
  CHECK(err.find("map 1") != std::string::npos && err.find("allow_knife") != std::string::npos);
  CHECK(fs::ValidateAssign(with(R"({"ruleset":"valve","overrides":{"allow_knife":true}})", nullptr), &err));
  // Bad ruleset / unknown or bad override keys: refused, the key named.
  CHECK(!fs::ValidateAssign(with(R"({"ruleset":"esl"})", veto), &err) && err.find("rules.ruleset") != std::string::npos);
  CHECK(!fs::ValidateAssign(with(R"({"ruleset":"valve","overrides":{"freeztime":3}})", veto), &err));
  CHECK(err.find("rules.overrides: unknown override \"freeztime\"") != std::string::npos);
  CHECK(!fs::ValidateAssign(with(R"({"overrides":{"overtime":{"limit":-1}}})", veto), &err));
  CHECK(err.find("overtime.limit") != std::string::npos);

  const Json q = with(R"({"ruleset":"valve","overrides":{"freezetime":3,"overtime":{"startmoney":12500}}})", veto);
  CHECK(fs::ValidateAssign(q, &err));
  const Json mat = fs::AssignToMatConfig("ko-r1-m3", *q.Find("config"), nullptr);
  CHECK_STR(mat.Find("config")->Find("ruleset")->AsString(), "valve");
  CHECK(mat.Find("config")->Find("coaches")->Items().size() == 1);
  auto ctx = ParseWebhookMatchContextFromJson(mat.Dump(), &err);
  CHECK(ctx.has_value());
  if (ctx) {
    CHECK_STR(ctx->ruleset, "valve");
    CHECK_STR(ctx->overrides_json, R"({"freezetime":3,"overtime":{"startmoney":12500}})");
    CHECK(ctx->coaches.count(76561198000000003ull) == 1);
    CHECK(ctx->spectators.count(76561198000000003ull) == 0);  // online valve match: no coach
    CHECK(ctx->maxOvertimes == -1 && !ctx->damageTiebreakEnabled);
  }
  // match.update set_rules is validated the same way.
  Json cfg = *q.Find("config");
  bool pw = false;
  CHECK(!fs::ApplyUpdateOps(&cfg, J(R"([{"op":"set_rules","rules":{"overrides":{"bogus":1}}}])"), &err, &pw));
  CHECK(err.find("bogus") != std::string::npos);
  CHECK(fs::ApplyUpdateOps(&cfg, J(R"([{"op":"set_rules","rules":{"overrides":{"lan":true}}}])"), &err, &pw));
}

static void TestUpdateOps() {
  Json cfg = *J(kAssign).Find("config");
  std::string err;
  bool pw = false;
  CHECK(fs::ApplyUpdateOps(&cfg, J(R"([
      {"op":"add_player","team":"team2","steamid64":"76561198000000012","name":"b2"},
      {"op":"add_player","team":"team1","steamid64":"76561198000000011","name":"b1-moved"},
      {"op":"remove_player","steamid64":"76561198000000099"},
      {"op":"rename_team","team":"team2","name":"Bravo Esports"},
      {"op":"set_password","password":"n3w"},
      {"op":"set_rules","rules":{"max_rounds":12,"knife":{"side_pick_seconds":30}}}])"),
                            &err, &pw));
  CHECK(pw);
  CHECK_STR(fs::TeamOf(cfg, 76561198000000011ull), "team1");  // moved, not duplicated
  CHECK_STR(fs::TeamOf(cfg, 76561198000000012ull), "team2");
  CHECK_STR(fs::TeamOf(cfg, 76561198000000099ull), "");
  CHECK_STR(cfg.Find("team2")->Find("name")->AsString(), "Bravo Esports");
  CHECK_STR(cfg.Find("password")->AsString(), "n3w");
  CHECK(cfg.Find("rules")->Find("max_rounds")->AsInt() == 12);
  CHECK(cfg.Find("rules")->Find("overtime")->Find("rounds_per_half")->AsInt() == 3);  // merged, not replaced
  CHECK(cfg.Find("rules")->Find("knife")->Find("side_pick_seconds")->AsInt() == 30);

  // A bad op leaves the config untouched (all or nothing).
  const std::string before = cfg.Dump();
  CHECK(!fs::ApplyUpdateOps(&cfg, J(R"([{"op":"rename_team","team":"team1","name":"ok"},
                                       {"op":"remove_player","steamid64":"76561198000000999"}])"),
                            &err, &pw));
  CHECK_STR(cfg.Dump(), before);
  CHECK(!fs::ApplyUpdateOps(&cfg, J(R"([{"op":"set_password","password":"a b"}])"), &err, &pw));
  CHECK(!fs::ApplyUpdateOps(&cfg, J(R"([{"op":"explode"}])"), &err, &pw));
  CHECK(!fs::ApplyUpdateOps(&cfg, J(R"([])"), &err, &pw));
}

// ---------------------------------------------------------------------------- codecs / validators

static void TestCodecs() {
  CHECK_STR(fs::Sha256Hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK_STR(fs::Sha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK_STR(fs::Sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  std::string million(1000000, 'a');
  CHECK_STR(fs::Sha256Hex(million), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

  CHECK_STR(fs::Base64Encode(""), "");
  CHECK_STR(fs::Base64Encode("f"), "Zg==");
  CHECK_STR(fs::Base64Encode("fo"), "Zm8=");
  CHECK_STR(fs::Base64Encode("foo"), "Zm9v");
  CHECK_STR(fs::Base64Encode("foobar"), "Zm9vYmFy");
  std::string bin;
  for (int i = 0; i < 256; ++i) bin.push_back(static_cast<char>(i));
  std::string back;
  CHECK(fs::Base64Decode(fs::Base64Encode(bin), &back) && back == bin);
  CHECK(fs::Base64Decode("Zm8=", &back) && back == "fo");
  CHECK(!fs::Base64Decode("Zm8=x", &back));
  CHECK(!fs::Base64Decode("Z!8=", &back));
}

static void TestValidators() {
  CHECK_STR(fs::SanitizeSay("hi\x01 there\n"), "hi there");
  CHECK(fs::SanitizeSay(std::string(300, 'x')).size() == 190);
  std::string utf;
  for (int i = 0; i < 100; ++i) utf += "\xc3\xa6";  // 'æ' x 100 = 200 bytes
  const std::string cut = fs::SanitizeSay(utf);
  CHECK(cut.size() == 190);
  CHECK((static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80);  // ends after a whole character

  std::string err;
  CHECK(fs::ValidateExec("mp_roundtime 2", &err));
  CHECK(fs::ValidateExec("status; mp_restartgame 1", &err));
  CHECK(!fs::ValidateExec("", &err));
  CHECK(!fs::ValidateExec(std::string(513, 'a'), &err));
  CHECK(!fs::ValidateExec("say a\nquit", &err));
  CHECK(!fs::ValidateExec("ru fleet enroll x", &err));
  CHECK(!fs::ValidateExec("status;  FLEET reconnect", &err));
  CHECK(!fs::ValidateExec("\"ru\" \"fleet\" status", &err));
  CHECK(fs::ValidateExec("ru status", &err));

  CHECK(fs::ValidPassword(""));
  CHECK(fs::ValidPassword("Ab-12_x!"));
  CHECK(!fs::ValidPassword("a b"));
  CHECK(!fs::ValidPassword("a\"b"));
  CHECK(!fs::ValidPassword(std::string(65, 'a')));
  CHECK(fs::SafeMapName("de_dust2"));
  CHECK(fs::SafeMapName("workshop/3070284539/de_nuke"));
  CHECK(!fs::SafeMapName("de_dust2 ; quit"));
  CHECK(!fs::SafeMapName("../../etc"));
  CHECK(fs::SafeWorkshopId("3070284539"));
  CHECK(!fs::SafeWorkshopId("30x"));

  CHECK(fs::BackupRoundFromName("readyup_backup_12_map1_round07.txt") == 7);
  CHECK(fs::BackupRoundFromName("backup_round00.txt") == 0);
  CHECK(fs::BackupRoundFromName("readyup_backup_12_map1_.txt") == -1);
  CHECK(fs::SafeBackupFileName("readyup_backup_12_map1_round07.txt"));
  CHECK(!fs::SafeBackupFileName("../x_round07.txt"));
  CHECK(!fs::SafeBackupFileName("x_round07.cfg"));
  CHECK(!fs::SafeBackupFileName(".hidden.txt"));
}

// ---------------------------------------------------------------------------- stats rewind

constexpr uint64_t A1 = 76561198000000001ull, A2 = 76561198000000002ull;
constexpr uint64_t B1 = 76561198000000011ull, B2 = 76561198000000012ull;

static void ObserveAll(stats::StatsAccumulator& st, bool team1Ct) {
  const int s1 = team1Ct ? 3 : 2, s2 = team1Ct ? 2 : 3;
  st.ObservePlayer(A1, "A1", s1, 1);
  st.ObservePlayer(A2, "A2", s1, 1);
  st.ObservePlayer(B1, "B1", s2, 2);
  st.ObservePlayer(B2, "B2", s2, 2);
}

// Round 1: team1 wins (A1 entry kill, A1 2k). Round 2: team2 wins (B1 3k incl. a clutch-free
// entry, B2 MVP). Round 3 (after a side swap): team2 wins by time.
static void PlayRound(stats::StatsAccumulator& st, int n, double t0) {
  st.OnRoundStart();
  if (n == 1) {
    ObserveAll(st, true);
    st.OnPlayerHurt(B1, A1, 100, 0, "ak47");
    st.OnPlayerDeath(B1, A1, 0, false, true, "ak47", t0 + 1);
    st.OnPlayerHurt(B2, A1, 100, 0, "ak47");
    st.OnPlayerDeath(B2, A1, A2, false, false, "ak47", t0 + 20);
    st.OnRoundMvp(A1);
    st.OnRoundEnd(3, 8);
  } else if (n == 2) {
    ObserveAll(st, true);
    st.OnPlayerHurt(A1, B1, 100, 0, "hegrenade");
    st.OnPlayerDeath(A1, B1, 0, false, false, "hegrenade", t0 + 1);
    st.OnPlayerHurt(A2, B1, 100, 0, "m4a1");
    st.OnPlayerDeath(A2, B1, B2, true, true, "m4a1", t0 + 30);
    st.OnRoundMvp(B2);
    st.OnRoundEnd(2, 9);
  } else {
    st.SetTeam1IsCt(false);
    ObserveAll(st, false);
    st.OnPlayerHurt(B1, A2, 40, 60, "glock");
    st.OnRoundEnd(3, 12);
  }
}

static void TestRewind() {
  stats::StatsAccumulator full, oneRound;
  full.BeginMap(true);
  oneRound.BeginMap(true);
  for (int n = 1; n <= 3; ++n) PlayRound(full, n, n * 100.0);
  PlayRound(oneRound, 1, 100.0);

  stats::MapStats m = full.Snapshot();
  CHECK(m.rounds.size() == 3);
  CHECK(stats::RewindTo(&m, 2) == 2);
  const stats::MapStats want = oneRound.Snapshot();
  CHECK(m.rounds.size() == 1);
  CHECK(m.team1_is_ct == true);  // back to the side of round 2
  CHECK(m.team1.score == want.team1.score && m.team1.score_ct == want.team1.score_ct);
  CHECK(m.team2.score == want.team2.score && m.team2.score_t == want.team2.score_t &&
        m.team2.score_ct == want.team2.score_ct);
  for (const auto& p : want.players) {
    const stats::PlayerLine* got = nullptr;
    for (const auto& q : m.players) {
      if (q.id == p.id) got = &q;
    }
    CHECK(got != nullptr);
    if (got) CHECK_STR(stats::ToJson(got->stats), stats::ToJson(p.stats));
  }
  // Rewinding to round 1 clears everything the rounds carried; nothing past the end is dropped.
  stats::MapStats all = full.Snapshot();
  CHECK(stats::RewindTo(&all, 1) == 3);
  CHECK(all.rounds.empty() && all.team1.score == 0 && all.team2.score == 0);
  for (const auto& p : all.players) {
    CHECK(p.stats.kills == 0 && p.stats.deaths == 0 && p.stats.rounds_played == 0 && p.stats.damage == 0);
  }
  stats::MapStats same = full.Snapshot();
  CHECK(stats::RewindTo(&same, 4) == 0);
  CHECK(stats::RewindTo(nullptr, 1) == 0);

  // A restored accumulator continues from the rewound snapshot.
  stats::StatsAccumulator resumed;
  resumed.Restore(m);
  CHECK(resumed.Team1Score() == want.team1.score);
}

// ---------------------------------------------------------------------------- workshop maps

static void TestMapNames() {
  namespace mn = readyup::mapnames;
  mn::ResetBindings();
  mn::MapRef r;
  CHECK(mn::ParseEntry("de_dust2", &r) && r.workshop_id.empty() && r.name == "de_dust2");
  CHECK(mn::ParseEntry("3084291314", &r) && r.workshop_id == "3084291314" && r.name.empty());
  CHECK(mn::ParseEntry("ws:3084291314", &r) && r.workshop_id == "3084291314" && r.name.empty());
  CHECK(mn::ParseEntry("workshop/3084291314", &r) && r.workshop_id == "3084291314" && r.name.empty());
  CHECK(mn::ParseEntry("workshop/3084291314/aim_map", &r) && r.workshop_id == "3084291314" && r.name == "aim_map");
  CHECK(!mn::ValidEntry(""));
  CHECK(!mn::ValidEntry("ws:"));
  CHECK(!mn::ValidEntry("ws:12a"));
  CHECK(!mn::ValidEntry("workshop/x/aim_map"));
  CHECK(!mn::ValidEntry("workshop/123/../x"));
  CHECK(!mn::ValidEntry("de_dust2;quit"));
  CHECK(!mn::ValidEntry("de dust2"));
  CHECK(!mn::ValidEntry(std::string(21, '1')));  // > 20 digits

  // Entries from match.assign maps[] {name, workshop_id}.
  CHECK_STR(mn::MakeEntry("de_nuke", ""), "de_nuke");
  CHECK_STR(mn::MakeEntry("aim_map", "3084291314"), "workshop/3084291314/aim_map");
  CHECK_STR(mn::MakeEntry("ws:3084291314", ""), "ws:3084291314");
  CHECK_STR(mn::MakeEntry("ws:1", "3084291314"), "workshop/3084291314");  // the id field wins
  CHECK_STR(mn::MakeEntry("", "3084291314"), "workshop/3084291314");
  CHECK_STR(mn::MakeEntry("bad name", ""), "");
  CHECK_STR(mn::MakeEntry("x", "12y"), "");

  CHECK_STR(mn::LoadCommand("de_dust2"), "changelevel de_dust2");
  CHECK_STR(mn::LoadCommand("3084291314"), "host_workshop_map 3084291314");
  CHECK_STR(mn::LoadCommand("ws:3084291314"), "host_workshop_map 3084291314");
  CHECK_STR(mn::LoadCommand("workshop/3084291314/aim_map"), "host_workshop_map 3084291314");
  CHECK_STR(mn::LoadCommand("de_dust2; quit"), "");

  // Loaded map names: the bsp name (CS2 1.41 logs `Loading map "aim_map"` after
  // host_workshop_map) or "workshop/<id>/<name>".
  CHECK_STR(mn::LoadedBaseName("aim_map"), "aim_map");
  CHECK_STR(mn::LoadedBaseName("workshop/3084291314/aim_map"), "aim_map");
  CHECK_STR(mn::LoadedBaseName("maps/aim_map.vpk"), "aim_map");
  CHECK_STR(mn::LoadedBaseName("workshop/3084291314"), "");
  CHECK_STR(mn::WorkshopIdOfLoaded("workshop/3084291314/aim_map"), "3084291314");
  CHECK_STR(mn::WorkshopIdOfLoaded("aim_map"), "");

  CHECK(mn::EntryMatchesLoaded("de_dust2", "de_dust2"));
  CHECK(mn::EntryMatchesLoaded("de_dust2", "DE_DUST2"));
  CHECK(!mn::EntryMatchesLoaded("de_dust2", "de_nuke"));
  CHECK(!mn::EntryMatchesLoaded("de_dust2", ""));
  CHECK(mn::EntryMatchesLoaded("workshop/3084291314/aim_map", "aim_map"));
  CHECK(mn::EntryMatchesLoaded("workshop/3084291314/aim_map", "workshop/3084291314/aim_map"));
  CHECK(!mn::EntryMatchesLoaded("workshop/3084291314/aim_map", "workshop/999/aim_map"));
  CHECK(mn::EntryMatchesLoaded("ws:3084291314", "workshop/3084291314/aim_map"));
  // An id-only entry matches a bare bsp name once its host_workshop_map loaded that map.
  CHECK(!mn::EntryMatchesLoaded("3084291314", "aim_map"));
  CHECK_STR(mn::DisplayName("3084291314"), "workshop/3084291314");
  mn::NoteWorkshopLoad("3084291314");
  mn::NoteMapLoaded("aim_map");
  CHECK_STR(mn::BoundName("3084291314"), "aim_map");
  CHECK(mn::EntryMatchesLoaded("3084291314", "aim_map"));
  CHECK(mn::EntryMatchesLoaded("ws:3084291314", "aim_map"));
  CHECK(!mn::EntryMatchesLoaded("3084291314", "de_dust2"));
  CHECK_STR(mn::DisplayName("3084291314"), "aim_map");
  CHECK_STR(mn::DisplayName("workshop/3084291314/aim_map"), "aim_map");
  CHECK_STR(mn::DisplayName("de_dust2"), "de_dust2");
  // The next map change without a pending load binds nothing; a "workshop/<id>/x" name binds itself.
  mn::NoteMapLoaded("de_dust2");
  CHECK_STR(mn::BoundName("3084291314"), "aim_map");
  mn::NoteMapLoaded("workshop/3070244462/aim_botz");
  CHECK_STR(mn::BoundName("3070244462"), "aim_botz");
  // A pending load overtaken by another workshop map does not bind the wrong name.
  mn::NoteWorkshopLoad("111");
  mn::NoteMapLoaded("workshop/222/other");
  CHECK_STR(mn::BoundName("111"), "");
  CHECK_STR(mn::BoundName("222"), "other");
  mn::ResetBindings();
  CHECK_STR(mn::BoundName("3084291314"), "");

  // match.assign: workshop maps in the maplist the MAT parser takes, "ws:<id>" names accepted.
  const Json p = J(R"({"match_id":"ws-1","epoch":1,"config":{"num_maps":3,
    "maps":[{"name":"ws:3084291314","sides":"knife"},{"name":"aim_map","workshop_id":"3084291314","sides":"team1_ct"},
            {"name":"workshop/3070244462","sides":"knife"}],
    "team1":{"name":"A","players":[]},"team2":{"name":"B","players":[]},"password":""}})");
  std::string err;
  CHECK(fs::ValidateAssign(p, &err));
  if (!err.empty()) std::fprintf(stderr, "validate: %s\n", err.c_str());
  const Json mat = fs::AssignToMatConfig("ws-1", *p.Find("config"), nullptr);
  auto ctx = ParseWebhookMatchContextFromJson(mat.Dump(), &err);
  CHECK(ctx.has_value());
  if (ctx) {
    CHECK(ctx->maplist.size() == 3);
    CHECK_STR(ctx->maplist[0], "ws:3084291314");
    CHECK_STR(ctx->maplist[1], "workshop/3084291314/aim_map");
    CHECK_STR(ctx->maplist[2], "workshop/3070244462");
  }
  Json bad = p;
  bad["config"]["maps"] = J(R"([{"name":"ws:abc","sides":"knife"}])");
  bad["config"]["num_maps"] = 1;
  CHECK(!fs::ValidateAssign(bad, &err));
}

// ---------------------------------------------------------------------------- failover resume

static Json ResumeConfig() {
  return J(R"({"num_maps":3,
    "maps":[{"name":"de_mirage","sides":"knife"},{"name":"de_inferno","sides":"knife"},{"name":"de_nuke","sides":"team2_ct"}],
    "team1":{"name":"A","players":[]},"team2":{"name":"B","players":[]},"password":"",
    "rules":{"pause":{"pause_after_restore":false}}})");
}

static Json InlineBackup(const std::string& raw, int map, int round) {
  Json b = Json::Object();
  b["map_number"] = map;
  b["round"] = round;
  b["file"] = "readyup_backup_77_map" + std::to_string(map) + "_round0" + std::to_string(round - 1) + ".txt";
  b["size"] = static_cast<long long>(raw.size());
  b["sha256"] = fs::Sha256Hex(raw);
  Json sc = Json::Object();
  sc["team1"] = 4;
  sc["team2"] = 2;
  b["score"] = sc;
  b["encoding"] = "base64";
  b["data"] = fs::Base64Encode(raw);
  return b;
}

static void TestResume() {
  const Json cfg = ResumeConfig();
  const std::string raw = "\"SaveFile\"\n{\n\t\"round\"\t\"7\"\n}\n";
  std::string code, err;
  fs::ResumePlan p;

  // Inline backup + the platform's state: series score, map 1's result, the knife-decided sides.
  Json r = J(R"({"from_epoch":2,"map_number":2,"round":7,
    "state":{"match_id":"m","epoch":2,"config_rev":1,"live_rev":40,"phase":"live",
      "series":{"num_maps":3,"current_map":2,"score":{"team1":1,"team2":0},
        "maps":{"1":{"name":"de_mirage","sides":"team1_ct","status":"done","score":{"team1":13,"team2":7},"winner":"team1"},
                "2":{"name":"de_inferno","sides":"team2_ct","status":"live","score":{"team1":4,"team2":2}},
                "3":{"name":"de_nuke","sides":"team2_ct","status":"pending"}}},
      "teams":{"team1":{"id":"a","name":"A","side":"t","score":4,"players":{}},
               "team2":{"id":"b","name":"B","side":"ct","score":2,"players":{}}}}})");
  r["backup"] = InlineBackup(raw, 2, 7);
  CHECK(fs::ParseResume(r, cfg, 3, &p, &code, &err));
  if (!err.empty()) std::fprintf(stderr, "resume: %s\n", err.c_str());
  CHECK(p.present && p.from_epoch == 2 && p.map_number == 2 && p.round == 7);
  CHECK(p.inline_backup && p.raw == raw && p.sha256 == fs::Sha256Hex(raw));
  CHECK(p.series_team1 == 1 && p.series_team2 == 0);
  CHECK(p.maps_done.size() == 1 && p.maps_done[0].map_number == 1 && p.maps_done[0].team1 == 13 &&
        p.maps_done[0].winner == "team1");
  CHECK_STR(p.sides, "team2_ct");
  CHECK(p.score_team1 == 4 && p.score_team2 == 2);  // backup.score
  CHECK_STR(p.team1_side, "t");
  CHECK(!p.pause_after_restore);

  // Round-trip for plugin reloads (the raw file is not kept).
  const fs::ResumePlan q = fs::ResumeFromJson(fs::ResumeToJson(p));
  CHECK(q.present && q.map_number == 2 && q.round == 7 && q.sha256 == p.sha256 && q.raw.empty());
  CHECK(q.maps_done.size() == 1 && q.maps_done[0].team1 == 13 && q.sides == "team2_ct" && !q.pause_after_restore);
  CHECK(q.score_team1 == 4 && q.team1_side == "t" && q.from_epoch == 2);

  // Explicit fields win over the state.
  Json r2 = r;
  r2["series_score"] = J(R"({"team1":0,"team2":1})");
  r2["maps"] = J(R"({"1":{"score":{"team1":8,"team2":13},"winner":"team2"}})");
  r2["sides"] = "team1_ct";
  r2["score"] = J(R"({"team1":3,"team2":3})");
  CHECK(fs::ParseResume(r2, cfg, 3, &p, &code, &err));
  CHECK(p.series_team2 == 1 && p.maps_done.size() == 1 && p.maps_done[0].winner == "team2");
  CHECK_STR(p.sides, "team1_ct");
  CHECK(p.score_team1 == 3 && p.score_team2 == 3);

  // Errors: codes the platform shows.
  auto rejects = [&](Json bad, long long epoch, const char* wantCode) {
    std::string c, e;
    const bool ok = fs::ParseResume(bad, cfg, epoch, nullptr, &c, &e);
    if (ok || c != wantCode) std::fprintf(stderr, "  resume case: ok=%d code=%s (%s)\n", ok, c.c_str(), e.c_str());
    return !ok && c == wantCode;
  };
  Json b = r;
  b["backup"]["sha256"] = std::string(64, '0');
  CHECK(rejects(b, 3, "checksum"));
  b = r;
  b["backup"]["size"] = 1;
  CHECK(rejects(b, 3, "checksum"));
  b = r;
  b["backup"]["parts"] = 2;
  CHECK(rejects(b, 3, "unsupported"));
  b = r;
  b["backup"]["data"] = "!!";
  CHECK(rejects(b, 3, "invalid_config"));
  b = r;
  b["backup"]["file"] = "../x_round06.txt";
  CHECK(rejects(b, 3, "invalid_config"));
  b = r;
  b["round"] = 6;  // not the backup's round
  CHECK(rejects(b, 3, "invalid_config"));
  b = r;
  b["map_number"] = 4;  // > num_maps
  CHECK(rejects(b, 3, "invalid_config"));
  b = r;
  b["from_epoch"] = 3;  // not older than the new epoch
  CHECK(rejects(b, 3, "invalid_config"));
  b = r;
  b["backup_ref"] = Json::Object();  // both
  CHECK(rejects(b, 3, "invalid_config"));
  // A knife map without decided sides cannot resume mid-map.
  b = J(R"({"map_number":1,"round":3,"backup_ref":{}})");
  CHECK(rejects(b, 3, "invalid_config"));
  b["sides"] = "team1_ct";
  CHECK(fs::ParseResume(b, cfg, 3, &p, &code, &err));
  CHECK(!p.inline_backup && p.file.empty() && p.round == 3 && p.series_team1 == 0);
  // backup_ref needs a round; its sha256 must look like one.
  CHECK(rejects(J(R"({"map_number":3,"backup_ref":{"file":"x_round01.txt"}})"), 3, "invalid_config"));
  CHECK(rejects(J(R"({"map_number":3,"round":2,"backup_ref":{"sha256":"xyz"}})"), 3, "invalid_config"));
  CHECK(fs::ParseResume(J(R"({"map_number":3,"round":2,"backup_ref":{"file":"x_round01.txt"}})"), cfg, 3, &p, &code, &err));
  CHECK_STR(p.file, "x_round01.txt");
  CHECK(p.sides.empty());  // map 3 has fixed sides in the config
  // No backup, no round: map 3 restarts from warmup (the knife map 1 could too).
  CHECK(fs::ParseResume(J(R"({"map_number":1,"series_score":{"team1":0,"team2":0}})"), cfg, 3, &p, &code, &err));
  CHECK(p.round == 0 && p.file.empty() && p.pause_after_restore == false);
  CHECK(rejects(J(R"({"map_number":1,"series_score":{"team1":-1,"team2":0}})"), 3, "invalid_config"));
  CHECK(rejects(J(R"([1])"), 3, "invalid_config"));
}

int main() {
  TestLiveStream();
  TestFence();
  TestAssign();
  TestUpdateOps();
  TestRulesetAssign();
  TestCodecs();
  TestValidators();
  TestRewind();
  TestMapNames();
  TestResume();
  if (g_failures) {
    std::fprintf(stderr, "fleet_state_test: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("fleet_state_test: all %d checks passed\n", g_checks);
  return 0;
}
