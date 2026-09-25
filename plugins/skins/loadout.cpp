// Loadout cache + DB access for readyup-skins.
//
// Tables are web-managed (docs/db-contract.md); the plugin only reads them, creates them if
// missing and bumps StatTrak counters. Every query runs on one worker thread that the plugin
// starts in load and joins in unload, so a reload never leaves a thread running in unmapped
// code. Lookups from the game thread only read the in-memory cache.
#include "skins.h"

#include "readyup/db_config.h"
#include "readyup/pg_client.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace skins {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::chrono::seconds kTtl{45};

struct CacheEntry {
  Loadout loadout;
  Clock::time_point loaded_at{};
  bool loading = false;
};

std::mutex g_mu;  // guards g_cache
std::unordered_map<uint64_t, CacheEntry> g_cache;

// ---- worker ---------------------------------------------------------------------------
std::mutex g_qmu;
std::condition_variable g_qcv;
std::deque<std::function<void()>> g_jobs;
bool g_stop = false;
std::thread g_worker;
bool g_running = false;
std::unique_ptr<readyup::pgc::Client> g_db;
bool g_schemaOk = false;  // worker thread only
std::string g_status = "not started";

void Post(std::function<void()> job) {
  {
    std::lock_guard<std::mutex> lk(g_qmu);
    if (!g_running || g_stop || g_jobs.size() >= 4096) return;
    g_jobs.push_back(std::move(job));
  }
  g_qcv.notify_one();
}

void WorkerMain() {
  for (;;) {
    std::function<void()> job;
    {
      std::unique_lock<std::mutex> lk(g_qmu);
      g_qcv.wait(lk, [] { return g_stop || !g_jobs.empty(); });
      if (g_stop) return;  // pending jobs are dropped; unload must not wait on the network
      job = std::move(g_jobs.front());
      g_jobs.pop_front();
    }
    try {
      job();
    } catch (...) {
      Log(RU_LOG_ERROR, "loadout worker: job threw; ignored");
    }
  }
}

// ---- queries (worker thread) -------------------------------------------------------------
int ToInt(const std::string& s) { return s.empty() ? 0 : std::atoi(s.c_str()); }
bool ToBool(const std::string& s) { return s == "t" || s == "true" || s == "1"; }

bool EnsureSchema(std::string* err) {
  if (g_schemaOk) return true;
  static const char* kDdl[] = {
      "CREATE TABLE IF NOT EXISTS readyup_weapon_skins ("
      "  steamid64 BIGINT NOT NULL,"
      "  weapon_team SMALLINT NOT NULL,"
      "  weapon_defindex INT NOT NULL,"
      "  paint_id INT NOT NULL,"
      "  wear REAL NOT NULL DEFAULT 0.000001,"
      "  seed INT NOT NULL DEFAULT 0,"
      "  nametag TEXT NULL,"
      "  stattrak_enabled BOOLEAN NOT NULL DEFAULT FALSE,"
      "  stattrak_count INT NOT NULL DEFAULT 0,"
      "  updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),"
      "  PRIMARY KEY (steamid64, weapon_team, weapon_defindex)"
      ");",
      "CREATE TABLE IF NOT EXISTS readyup_weapon_knives ("
      "  steamid64 BIGINT NOT NULL,"
      "  weapon_team SMALLINT NOT NULL,"
      "  knife_classname TEXT NOT NULL,"
      "  updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),"
      "  PRIMARY KEY (steamid64, weapon_team)"
      ");",
      "CREATE TABLE IF NOT EXISTS readyup_weapon_gloves ("
      "  steamid64 BIGINT NOT NULL,"
      "  weapon_team SMALLINT NOT NULL,"
      "  glove_defindex INT NOT NULL,"
      "  updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),"
      "  PRIMARY KEY (steamid64, weapon_team)"
      ");",
      "CREATE TABLE IF NOT EXISTS readyup_weapon_agents ("
      "  steamid64 BIGINT PRIMARY KEY,"
      "  agent_ct TEXT NULL,"
      "  agent_t TEXT NULL,"
      "  updated_at TIMESTAMPTZ NOT NULL DEFAULT now()"
      ");",
  };
  for (const char* ddl : kDdl) {
    if (!g_db->Exec(ddl, {}, nullptr, err)) return false;
  }
  g_schemaOk = true;
  SKINS_DEBUG("db: weapon paints schema OK");
  return true;
}

bool Query(const char* what, uint64_t sid, const char* sql, readyup::pgc::Result* out) {
  std::string err;
  if (g_db->Exec(sql, {std::to_string(sid)}, out, &err)) return true;
  SKINS_DEBUG("db: %s(%llu) failed: %s", what, static_cast<unsigned long long>(sid), err.c_str());
  return false;
}

void MergeInto(Loadout& l, uint64_t sid) {
  readyup::pgc::Result r;
  if (Query("skins", sid,
            "SELECT weapon_team, weapon_defindex, paint_id, wear, seed, COALESCE(nametag, ''), stattrak_enabled, "
            "stattrak_count FROM readyup_weapon_skins WHERE steamid64 = $1::bigint "
            "ORDER BY weapon_team ASC, weapon_defindex ASC;",
            &r)) {
    for (const auto& row : r.rows) {
      if (row.size() < 8) continue;
      WeaponSkinEntry e;
      e.weapon_team = ToInt(row[0]);
      e.weapon_defindex = ToInt(row[1]);
      e.paint_id = ToInt(row[2]);
      e.wear = static_cast<float>(std::strtod(row[3].c_str(), nullptr));
      e.seed = ToInt(row[4]);
      e.nametag = row[5];
      e.stattrak_enabled = ToBool(row[6]);
      e.stattrak_count = ToInt(row[7]);
      l.by_team[e.weapon_team].skins_by_defindex[e.weapon_defindex] = e;
    }
  }
  if (Query("knives", sid,
            "SELECT weapon_team, knife_classname FROM readyup_weapon_knives WHERE steamid64 = $1::bigint "
            "ORDER BY weapon_team ASC;",
            &r)) {
    for (const auto& row : r.rows) {
      if (row.size() < 2 || row[1].empty()) continue;
      l.by_team[ToInt(row[0])].knife_classname = row[1];
    }
  }
  if (Query("gloves", sid,
            "SELECT weapon_team, glove_defindex FROM readyup_weapon_gloves WHERE steamid64 = $1::bigint "
            "ORDER BY weapon_team ASC;",
            &r)) {
    for (const auto& row : r.rows) {
      if (row.size() < 2 || ToInt(row[1]) <= 0) continue;
      l.by_team[ToInt(row[0])].glove_defindex = ToInt(row[1]);
    }
  }
  if (Query("agents", sid,
            "SELECT COALESCE(agent_ct, ''), COALESCE(agent_t, '') FROM readyup_weapon_agents "
            "WHERE steamid64 = $1::bigint LIMIT 1;",
            &r) &&
      !r.rows.empty() && r.rows[0].size() >= 2) {
    l.agents = WeaponAgentEntry{r.rows[0][0], r.rows[0][1]};
  }
}

void RefreshJob(uint64_t sid) {
  Loadout l;
  std::string err;
  bool ok = EnsureSchema(&err);
  if (!ok) {
    SKINS_DEBUG("db: ensure schema failed: %s", err.c_str());
  } else {
    MergeInto(l, sid);
  }
  std::lock_guard<std::mutex> lk(g_mu);
  CacheEntry& e = g_cache[sid];
  e.loading = false;
  if (!ok) return;  // keep serving whatever we had; retried after the TTL
  e.loadout = std::move(l);
  e.loaded_at = Clock::now();
}

const TeamLoadout* TeamLocked(const Loadout& l, int team) {
  auto it = l.by_team.find(team);
  return it == l.by_team.end() ? nullptr : &it->second;
}

// Team-specific rows (2=T, 3=CT) win; team 0 ("both") is the per-field fallback.
std::optional<TeamLoadout> FindTeamLocked(const Loadout& l, int weapon_team) {
  const TeamLoadout* specific = TeamLocked(l, weapon_team);
  const TeamLoadout* both = weapon_team != 0 ? TeamLocked(l, 0) : nullptr;
  if (!specific && !both) return std::nullopt;
  TeamLoadout out = both ? *both : TeamLoadout{};
  if (specific) {
    for (const auto& kv : specific->skins_by_defindex) out.skins_by_defindex[kv.first] = kv.second;
    if (specific->knife_classname) out.knife_classname = specific->knife_classname;
    if (specific->glove_defindex) out.glove_defindex = specific->glove_defindex;
  }
  return out;
}

bool IsStale(const CacheEntry& e, Clock::time_point now) {
  return e.loaded_at.time_since_epoch().count() == 0 || (now - e.loaded_at) > kTtl;
}

}  // namespace

bool LoadoutStart() {
  const char* dir = g_api->config_dir(g_api->self);
  const std::string path = std::string(dir ? dir : "") + "/readyup_db.json";
  std::string err;
  const auto cfg = readyup::ReadDbConfigFile(path, &err);
  if (!readyup::pgc::Compiled()) {
    g_status = "built without Postgres";
    return false;
  }
  if (!cfg) {
    g_status = "no DB (" + err + ")";
    return false;
  }
  g_db = std::make_unique<readyup::pgc::Client>([](bool debug, const std::string& line) {
    if (debug) SKINS_DEBUG("%s", line.c_str());
    else Log(RU_LOG_INFO, "%s", line.c_str());
  });
  g_db->Configure(cfg->conninfo, cfg->conninfo_sanitized);
  g_schemaOk = false;
  {
    std::lock_guard<std::mutex> lk(g_qmu);
    g_stop = false;
    g_jobs.clear();
    g_running = true;
  }
  g_worker = std::thread(WorkerMain);
  g_status = "db " + cfg->conninfo_sanitized;
  Post([] {
    std::string e;
    if (!EnsureSchema(&e)) SKINS_DEBUG("db: ensure schema failed: %s", e.c_str());
  });
  return true;
}

void LoadoutStop() {
  {
    std::lock_guard<std::mutex> lk(g_qmu);
    g_stop = true;
    g_jobs.clear();
  }
  g_qcv.notify_all();
  if (g_worker.joinable()) g_worker.join();
  {
    std::lock_guard<std::mutex> lk(g_qmu);
    g_running = false;
  }
  g_db.reset();
  std::lock_guard<std::mutex> lk(g_mu);
  g_cache.clear();
}

std::string LoadoutStatus() {
  size_t cached = 0;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    cached = g_cache.size();
  }
  size_t queued = 0;
  {
    std::lock_guard<std::mutex> lk(g_qmu);
    queued = g_jobs.size();
  }
  return g_status + ", " + std::to_string(cached) + " player(s) cached, " + std::to_string(queued) + " job(s) queued";
}

void MaybeRefreshAsync(uint64_t steamid64) {
  if (steamid64 == 0) return;
  const auto now = Clock::now();
  {
    std::lock_guard<std::mutex> lk(g_mu);
    CacheEntry& e = g_cache[steamid64];
    if (e.loading || !IsStale(e, now)) return;
    e.loading = true;
  }
  Post([steamid64] { RefreshJob(steamid64); });
}

void Invalidate(uint64_t steamid64) {
  if (steamid64 == 0) return;
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_cache.find(steamid64);
  if (it == g_cache.end()) return;
  // Keep serving the old loadout until the refetch lands; just force the next refresh.
  it->second.loaded_at = Clock::time_point{} + std::chrono::nanoseconds(1);
}

bool IsLoaded(uint64_t steamid64) {
  if (steamid64 == 0) return false;
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_cache.find(steamid64);
  return it != g_cache.end() && it->second.loaded_at.time_since_epoch().count() != 0;
}

std::optional<WeaponSkinEntry> FindWeaponSkin(uint64_t steamid64, int weapon_team, int weapon_defindex) {
  if (steamid64 == 0 || weapon_defindex <= 0) return std::nullopt;
  MaybeRefreshAsync(steamid64);
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_cache.find(steamid64);
  if (it == g_cache.end()) return std::nullopt;
  const auto tl = FindTeamLocked(it->second.loadout, weapon_team);
  if (!tl) return std::nullopt;
  auto ws = tl->skins_by_defindex.find(weapon_defindex);
  if (ws == tl->skins_by_defindex.end()) return std::nullopt;
  return ws->second;
}

std::optional<std::string> FindKnifeClassname(uint64_t steamid64, int weapon_team) {
  if (steamid64 == 0) return std::nullopt;
  MaybeRefreshAsync(steamid64);
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_cache.find(steamid64);
  if (it == g_cache.end() || it->second.loaded_at.time_since_epoch().count() == 0) return std::nullopt;
  const auto tl = FindTeamLocked(it->second.loadout, weapon_team);
  // No knife row: stock knife (no default, not even for admins).
  if (tl && tl->knife_classname && !tl->knife_classname->empty()) return *tl->knife_classname;
  return std::nullopt;
}

std::optional<int> FindGloveDefindex(uint64_t steamid64, int weapon_team) {
  if (steamid64 == 0) return std::nullopt;
  MaybeRefreshAsync(steamid64);
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_cache.find(steamid64);
  if (it == g_cache.end()) return std::nullopt;
  const auto tl = FindTeamLocked(it->second.loadout, weapon_team);
  if (!tl || !tl->glove_defindex || *tl->glove_defindex <= 0) return std::nullopt;
  return *tl->glove_defindex;
}

std::optional<WeaponAgentEntry> FindAgents(uint64_t steamid64) {
  if (steamid64 == 0) return std::nullopt;
  MaybeRefreshAsync(steamid64);
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_cache.find(steamid64);
  if (it == g_cache.end() || !it->second.loadout.agents) return std::nullopt;
  const auto& a = *it->second.loadout.agents;
  if (a.agent_ct.empty() && a.agent_t.empty()) return std::nullopt;
  return a;
}

std::optional<int> KnifeClassnameToDefindex(const std::string& classname) {
  // Item defindexes from items_game.txt (CS2 1.41.8.3). Default knives map to nullopt (no change).
  static const std::unordered_map<std::string, int> kKnives = {
      {"weapon_bayonet", 500},
      {"weapon_knife_css", 503},
      {"weapon_knife_flip", 505},
      {"weapon_knife_gut", 506},
      {"weapon_knife_karambit", 507},
      {"weapon_knife_m9_bayonet", 508},
      {"weapon_knife_tactical", 509},
      {"weapon_knife_falchion", 512},
      {"weapon_knife_survival_bowie", 514},
      {"weapon_knife_butterfly", 515},
      {"weapon_knife_push", 516},
      {"weapon_knife_cord", 517},
      {"weapon_knife_canis", 518},
      {"weapon_knife_ursus", 519},
      {"weapon_knife_gypsy_jackknife", 520},
      {"weapon_knife_outdoor", 521},
      {"weapon_knife_stiletto", 522},
      {"weapon_knife_widowmaker", 523},
      {"weapon_knife_skeleton", 525},
      {"weapon_knife_kukri", 526},
  };
  auto it = kKnives.find(classname);
  if (it == kKnives.end()) return std::nullopt;
  return it->second;
}

void IncrementStatTrakAsync(uint64_t steamid64, int weapon_team, int weapon_defindex) {
  Post([steamid64, weapon_team, weapon_defindex] {
    std::string err;
    readyup::pgc::Result r;
    const bool ok = EnsureSchema(&err) &&
                    g_db->Exec("UPDATE readyup_weapon_skins SET stattrak_count = stattrak_count + 1, updated_at = now() "
                               "WHERE steamid64 = $1::bigint AND weapon_team = $2::smallint AND weapon_defindex = $3::int;",
                               {std::to_string(steamid64), std::to_string(weapon_team), std::to_string(weapon_defindex)},
                               &r, &err);
    if (ok && r.affected > 0) {
      Invalidate(steamid64);  // next apply sees the new count
    } else {
      SKINS_DEBUG("stattrak: increment(%llu,%d,%d) failed: %s", static_cast<unsigned long long>(steamid64),
                  weapon_team, weapon_defindex, ok ? "row not found" : err.c_str());
    }
  });
}

}  // namespace skins
