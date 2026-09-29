// Offline tests for readyup/retention_rules.h: which old round backups, resume files and demos
// the retention sweep deletes. ctest `match_retention`.
#include "readyup/retention_rules.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace readyup::retention;

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

int main() {
  constexpr long long H = 3600;
  const long long now = 2000000000LL;

  // ---- names
  uint64_t id = 0;
  CHECK(ParseRetainedBackupName("readyup_backup_42_map1_round07.txt", &id) && id == 42);
  CHECK(ParseRetainedBackupName("readyup_resume_9001_map3_round12.txt", &id) && id == 9001);
  CHECK(!ParseRetainedBackupName("backup_round07.txt", &id));                    // CS2's default name
  CHECK(!ParseRetainedBackupName(".readyup_backup_42_map1_round07.txt.tmp", &id));  // resume temp file
  CHECK(!ParseRetainedBackupName("readyup_backup_42_map1_round07.txt.bak", &id));
  CHECK(!ParseRetainedBackupName("readyup_backup__map1_round07.txt", &id));
  CHECK(!ParseRetainedBackupName("readyup_backup_42_map1.txt", &id));
  CHECK(!ParseRetainedBackupName("readyup_backup_42_mapX_round07.txt", &id));
  CHECK(!ParseRetainedBackupName("readyup.cfg", &id));

  CHECK(DemoNameHasMatchId("2026-09-29_12-00-00_42_de_dust2_a_vs_b.dem", 42));
  CHECK(DemoNameHasMatchId("42_de_dust2.dem", 42));
  CHECK(DemoNameHasMatchId("match_42.dem", 42));
  CHECK(!DemoNameHasMatchId("2026-09-29_12-00-00_420_de_dust2_a_vs_b.dem", 42));
  CHECK(!DemoNameHasMatchId("2026-09-29_12-00-00_142_de_dust2_a_vs_b.dem", 42));
  CHECK(!DemoNameHasMatchId("anything_0_x.dem", 0));

  // ---- age
  CHECK(!Expired(now, now - 71 * H, 72));
  CHECK(Expired(now, now - 72 * H, 72));
  CHECK(!Expired(now, now - 10000 * H, 0));  // 0 = keep forever
  CHECK(!Expired(now, now + H, 1));          // clock skew: a future mtime is kept

  // ---- backups
  const std::vector<FileEntry> backups = {
      {"readyup_backup_1_map1_round03.txt", now - 100 * H},  // old, other match: deleted
      {"readyup_resume_1_map2_round00.txt", now - 100 * H},  // old resume copy: deleted
      {"readyup_backup_2_map1_round03.txt", now - 10 * H},   // young: kept
      {"readyup_backup_7_map1_round03.txt", now - 500 * H},  // the loaded match: kept
      {"readyup_resume_8_map1_round03.txt", now - 500 * H},  // the recovered match: kept
      {"backup_round03.txt", now - 500 * H},                 // not ours: kept
      {"gamestate.txt", now - 500 * H},
  };
  auto del = BackupsToDelete(backups, now, 72, {7, 8});
  CHECK(del.size() == 2);
  CHECK(Has(del, "readyup_backup_1_map1_round03.txt"));
  CHECK(Has(del, "readyup_resume_1_map2_round00.txt"));
  CHECK(BackupsToDelete(backups, now, 0, {7}).empty());
  // No match loaded: old files of any match go.
  CHECK(BackupsToDelete(backups, now, 72, {}).size() == 4);
  // A protected id of 0 (no match) protects nothing special.
  CHECK(BackupsToDelete(backups, now, 72, {0}).size() == 4);
  CHECK(BackupsToDelete(backups, now, 1, {7, 8}).size() == 3);

  // ---- demos
  const std::vector<FileEntry> demos = {
      {"2026-09-01_10-00-00_1_de_inferno_a_vs_b.dem", now - 48 * H},  // old: deleted
      {"2026-09-01_11-00-00_1_de_mirage_a_vs_b.dem", now - 23 * H},   // young: kept
      {"2026-09-01_12-00-00_7_de_nuke_a_vs_b.dem", now - 48 * H},     // loaded match (map 1): kept
      {"2026-09-29_12-00-00_3_de_ancient_c_vs_d.dem", now - 48 * H},  // recording (keepNames): kept
      {"notes.txt", now - 480 * H},                                   // not a demo
      {".dem", now - 480 * H},
  };
  const std::vector<std::string> keep = {"2026-09-29_12-00-00_3_de_ancient_c_vs_d.dem"};
  auto dd = DemosToDelete(demos, now, 24, {7}, keep);
  CHECK(dd.size() == 1);
  CHECK(Has(dd, "2026-09-01_10-00-00_1_de_inferno_a_vs_b.dem"));
  CHECK(DemosToDelete(demos, now, 0, {7}, keep).empty());  // 0 = keep
  CHECK(DemosToDelete(demos, now, 24, {}, {}).size() == 3);
  CHECK(DemosToDelete(demos, now, 72, {}, {}).empty());

  std::printf("retention_test: %s\n", g_failures ? "FAIL" : "PASS");
  return g_failures ? 1 : 0;
}
