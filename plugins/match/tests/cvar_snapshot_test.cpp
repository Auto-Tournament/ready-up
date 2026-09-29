// Offline tests for readyup/cvar_snapshot.h Snapshot: which match cvars get read, first value
// wins, the restore commands, and the JSON kept across a reload / restart.
// ctest `match_cvar_snapshot`.
#include "readyup/cvar_snapshot.h"

#include <cstdio>
#include <string>
#include <vector>

using readyup::cvar_snapshot::QuotableValue;
using readyup::cvar_snapshot::Snapshot;

static int g_failures = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

using V = std::vector<std::string>;

int main() {
  // Match load: every cvars{} name is read once (lowercase), before go-live sets it.
  Snapshot s;
  CHECK(s.Empty());
  CHECK((s.TakeKeysToQuery({"mp_maxrounds", "MP_Overtime_Enable", "tv_delay"}) ==
         V{"mp_maxrounds", "mp_overtime_enable", "tv_delay"}));
  CHECK(!s.Empty());
  // Applying the cvars again (go-live, map 2, match.update) asks only for new names.
  CHECK(s.TakeKeysToQuery({"mp_maxrounds", "tv_delay"}).empty());
  CHECK((s.TakeKeysToQuery({"mp_maxrounds", "mp_freezetime"}) == V{"mp_freezetime"}));

  // Answers: NULL (unknown / no answer) is not restored; the first value wins.
  s.Record("mp_maxrounds", "24");
  s.Record("MP_OVERTIME_ENABLE", "0");
  s.Record("tv_delay", nullptr);
  s.Record("mp_freezetime", "15");
  s.Record("mp_maxrounds", "30");  // a later read (the match value) never replaces the original
  CHECK(s.size() == 3);
  CHECK(s.values().at("mp_maxrounds") == "24");
  CHECK(s.TakeKeysToQuery({"tv_delay"}).empty());  // unknown stays asked: not read again

  // A second match over the first: names the first one read keep their original value.
  CHECK((s.TakeKeysToQuery({"mp_maxrounds", "sv_talk_enemy_dead"}) == V{"sv_talk_enemy_dead"}));
  s.Record("sv_talk_enemy_dead", "");  // empty string value
  // A query that could not go out may be asked again; one with a value may not.
  CHECK((s.TakeKeysToQuery({"mp_c4timer"}) == V{"mp_c4timer"}));
  s.Unask("mp_c4timer");
  s.Unask("mp_maxrounds");
  CHECK((s.TakeKeysToQuery({"mp_c4timer", "mp_maxrounds"}) == V{"mp_c4timer"}));

  // Series end: `name "value"` in name order; unquotable values are skipped by name.
  s.Record("hostname", "My \"quoted\" server");
  s.Record("sv_tags", "a;quit");
  V skipped;
  const V cmds = s.RestoreCommands(&skipped);
  CHECK((cmds == V{"mp_freezetime \"15\"", "mp_maxrounds \"24\"", "mp_overtime_enable \"0\"", "sv_talk_enemy_dead \"\""}));
  CHECK((skipped == V{"hostname", "sv_tags"}));
  CHECK(QuotableValue("de_dust2 night") && QuotableValue("") && !QuotableValue("a\nb") && !QuotableValue("x;y"));

  // Reload keeps values and asked names; a restart keeps values only.
  Snapshot r;
  CHECK(r.FromJson(s.ToJson(true), true));
  CHECK(r.values() == s.values());
  CHECK(r.TakeKeysToQuery({"mp_c4timer", "tv_delay", "mp_roundtime"}) == V{"mp_roundtime"});
  Snapshot boot;
  CHECK(boot.FromJson(s.ToJson(false), false));
  CHECK(boot.values() == s.values());
  CHECK((boot.TakeKeysToQuery({"mp_maxrounds", "mp_c4timer", "tv_delay"}) == V{"mp_c4timer", "tv_delay"}));
  CHECK(s.ToJson(false).find("asked") == std::string::npos);
  CHECK(!boot.FromJson("not json", false) && !boot.FromJson("[1]", false));

  s.Clear();
  CHECK(s.Empty() && s.RestoreCommands().empty());
  CHECK((s.TakeKeysToQuery({"mp_maxrounds"}) == V{"mp_maxrounds"}));  // the next series reads again

  std::printf("cvar_snapshot_test: %s\n", g_failures ? "FAIL" : "PASS");
  return g_failures ? 1 : 0;
}
