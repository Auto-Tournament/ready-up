// Offline tests for the engine-free half of the fleet match link (readyup/fleet_state.h) and the
// stats rewind used by round restores (match_stats.h RewindTo): merge patches and live_rev,
// epoch fencing, config_rev compare-and-set, match.assign -> MAT config (through the real
// parser), match.update ops, sha256 / base64, validators. ctest `match_fleet_state`.
#include "readyup/fleet_state.h"
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
    "team1": {"id":"t-a","name":"Alpha","tag":"ALP","captain":"76561198000000001",
              "players":[{"steamid64":"76561198000000001","name":"a1"},
                         {"steamid64":"76561198000000002","name":"a2","role":"sub"},
                         {"steamid64":"76561198000000003","name":"coachA","role":"coach"}]},
    "team2": {"id":"t-b","name":"Bravo","players":[{"steamid64":"76561198000000011","name":"b1"}]},
    "spectators": ["76561198000000099"],
    "admins": ["76561198000000077"],
    "password": "s3cret-pw",
    "rules": {"max_rounds": 24, "overtime": {"enabled": true, "rounds_per_half": 3, "max_overtimes": 2},
              "tiebreak": {"damage": true, "sudden_death_on_tie": false},
              "ready": {"min_per_team": 0, "allow_force_ready": true, "autoready": false},
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
  CHECK(ctx->maplist.size() == 3 && ctx->maplist[2] == "de_nuke");
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

int main() {
  TestLiveStream();
  TestFence();
  TestAssign();
  TestUpdateOps();
  TestCodecs();
  TestValidators();
  TestRewind();
  if (g_failures) {
    std::fprintf(stderr, "fleet_state_test: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("fleet_state_test: all %d checks passed\n", g_checks);
  return 0;
}
