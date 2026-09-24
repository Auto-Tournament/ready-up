#include "readyup/weapon_paints.h"

#include "readyup/admin_check.h"
#include "readyup/config.h"
#include "readyup/logging.h"
#include "readyup/postgres.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace readyup::weapon_paints {
namespace {

using Clock = std::chrono::steady_clock;

struct CacheEntry {
  Loadout loadout;
  Clock::time_point loaded_at{};
  bool loading = false;
};

std::mutex g_mu;
std::unordered_map<uint64_t, CacheEntry> g_cache;

static constexpr std::chrono::seconds kTtl{45};

static bool IsStale(const CacheEntry& e, Clock::time_point now) {
  if (e.loaded_at.time_since_epoch().count() == 0) return true;
  return (now - e.loaded_at) > kTtl;
}

static TeamLoadout& EnsureTeam(Loadout& l, int team) {
  return l.by_team[team];
}

static void MergeSkins(Loadout& l, const std::vector<readyup::WeaponSkinEntry>& skins) {
  for (const auto& s : skins) {
    TeamLoadout& tl = EnsureTeam(l, s.weapon_team);
    tl.skins_by_defindex[s.weapon_defindex] = s;
  }
}

static void MergeKnives(Loadout& l, const std::vector<readyup::WeaponKnifeEntry>& knives) {
  for (const auto& k : knives) {
    if (k.knife_classname.empty()) continue;
    TeamLoadout& tl = EnsureTeam(l, k.weapon_team);
    tl.knife_classname = k.knife_classname;
  }
}

static void MergeGloves(Loadout& l, const std::vector<readyup::WeaponGloveEntry>& gloves) {
  for (const auto& g : gloves) {
    if (g.glove_defindex <= 0) continue;
    TeamLoadout& tl = EnsureTeam(l, g.weapon_team);
    tl.glove_defindex = g.glove_defindex;
  }
}

static void RefreshWorker(uint64_t steamid64) {
  if (!pg::Available()) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_cache.find(steamid64);
    if (it != g_cache.end()) it->second.loading = false;
    return;
  }

  std::string err;
  if (!pg::EnsureWeaponPaintsSchema(&err)) {
    if (readyup::DebugEnabled() && !err.empty()) {
      readyup::Debug("weapon_paints: EnsureWeaponPaintsSchema failed: %s\n", err.c_str());
    }
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_cache.find(steamid64);
    if (it != g_cache.end()) it->second.loading = false;
    return;
  }

  err.clear();
  const auto skins = pg::ListWeaponSkins(steamid64, &err);
  if (readyup::DebugEnabled() && !err.empty()) {
    readyup::Debug("weapon_paints: ListWeaponSkins(%llu) err=%s\n",
                   static_cast<unsigned long long>(steamid64), err.c_str());
  }

  err.clear();
  const auto knives = pg::ListWeaponKnives(steamid64, &err);
  if (readyup::DebugEnabled() && !err.empty()) {
    readyup::Debug("weapon_paints: ListWeaponKnives(%llu) err=%s\n",
                   static_cast<unsigned long long>(steamid64), err.c_str());
  }

  err.clear();
  const auto gloves = pg::ListWeaponGloves(steamid64, &err);
  if (readyup::DebugEnabled() && !err.empty()) {
    readyup::Debug("weapon_paints: ListWeaponGloves(%llu) err=%s\n",
                   static_cast<unsigned long long>(steamid64), err.c_str());
  }

  err.clear();
  const auto agents = pg::GetWeaponAgents(steamid64, &err);
  if (readyup::DebugEnabled() && !err.empty()) {
    readyup::Debug("weapon_paints: GetWeaponAgents(%llu) err=%s\n",
                   static_cast<unsigned long long>(steamid64), err.c_str());
  }

  Loadout l;
  // Admin lookup may hit Postgres; do it here (worker thread), never on the game thread.
  l.is_admin = readyup::IsReadyUpAdmin(steamid64);
  MergeSkins(l, skins);
  MergeKnives(l, knives);
  MergeGloves(l, gloves);
  if (agents) l.agents = *agents;

  {
    std::lock_guard<std::mutex> lk(g_mu);
    CacheEntry& e = g_cache[steamid64];
    e.loadout = std::move(l);
    e.loaded_at = Clock::now();
    e.loading = false;
  }
}

static const TeamLoadout* TeamLocked(const Loadout& l, int team) {
  auto it = l.by_team.find(team);
  return it == l.by_team.end() ? nullptr : &it->second;
}

// Team-specific rows (2=T, 3=CT) win; team 0 ("both") is the per-field fallback.
static std::optional<TeamLoadout> FindTeamLocked(const Loadout& l, int weapon_team) {
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

}  // namespace

void EnsureSchemaAsync() {
  static std::once_flag once;
  std::call_once(once, [] {
    std::thread([] {
      std::string err;
      (void)pg::EnsureWeaponPaintsSchema(&err);
    }).detach();
  });
}

void MaybeRefreshAsync(uint64_t steamid64) {
  if (steamid64 == 0) return;
  EnsureSchemaAsync();

  const auto now = Clock::now();
  bool shouldStart = false;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    CacheEntry& e = g_cache[steamid64];
    if (!e.loading && IsStale(e, now)) {
      e.loading = true;
      shouldStart = true;
    }
  }

  if (!shouldStart) return;
  std::thread([steamid64] { RefreshWorker(steamid64); }).detach();
}

void Invalidate(uint64_t steamid64) {
  if (steamid64 == 0) return;
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_cache.find(steamid64);
  if (it == g_cache.end()) return;
  // Keep serving the old loadout until the refetch lands; just force the next refresh.
  it->second.loaded_at = Clock::time_point{} + std::chrono::nanoseconds(1);
}

std::optional<readyup::WeaponSkinEntry> FindWeaponSkin(uint64_t steamid64, int weapon_team, int weapon_defindex) {
  if (steamid64 == 0 || weapon_defindex <= 0) return std::nullopt;
  MaybeRefreshAsync(steamid64);

  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_cache.find(steamid64);
  if (it == g_cache.end()) return std::nullopt;
  const auto tl = FindTeamLocked(it->second.loadout, weapon_team);
  if (!tl) return std::nullopt;

  auto ws = tl->skins_by_defindex.find(weapon_defindex);
  if (ws != tl->skins_by_defindex.end()) return ws->second;

  // Fallback: if caller asks for a team-specific lookup but only team 0 exists
  // (or vice versa), FindTeamLocked already handled team 0. No further fallback.
  return std::nullopt;
}

std::optional<std::string> FindKnifeClassname(uint64_t steamid64, int weapon_team) {
  if (steamid64 == 0) return std::nullopt;
  MaybeRefreshAsync(steamid64);

  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_cache.find(steamid64);
  if (it == g_cache.end() || it->second.loaded_at.time_since_epoch().count() == 0) return std::nullopt;

  // If user configured a knife in DB, prefer it.
  const auto tl = FindTeamLocked(it->second.loadout, weapon_team);
  if (tl && tl->knife_classname && !tl->knife_classname->empty()) return *tl->knife_classname;

  // Tournament default: admins get a butterfly by default (only when DB has no knife).
  if (it->second.loadout.is_admin) return std::optional<std::string>{"weapon_knife_butterfly"};
  return std::nullopt;
}

bool IsLoaded(uint64_t steamid64) {
  if (steamid64 == 0) return false;
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_cache.find(steamid64);
  return it != g_cache.end() && it->second.loaded_at.time_since_epoch().count() != 0;
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

std::optional<int> FindGloveDefindex(uint64_t steamid64, int weapon_team) {
  if (steamid64 == 0) return std::nullopt;
  MaybeRefreshAsync(steamid64);

  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_cache.find(steamid64);
  if (it == g_cache.end()) return std::nullopt;
  const auto tl = FindTeamLocked(it->second.loadout, weapon_team);
  if (!tl) return std::nullopt;
  if (!tl->glove_defindex || *tl->glove_defindex <= 0) return std::nullopt;
  return *tl->glove_defindex;
}

std::optional<readyup::WeaponAgentEntry> FindAgents(uint64_t steamid64) {
  if (steamid64 == 0) return std::nullopt;
  MaybeRefreshAsync(steamid64);

  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_cache.find(steamid64);
  if (it == g_cache.end()) return std::nullopt;
  if (!it->second.loadout.agents) return std::nullopt;
  if (it->second.loadout.agents->agent_ct.empty() && it->second.loadout.agents->agent_t.empty()) return std::nullopt;
  return *it->second.loadout.agents;
}

}  // namespace readyup::weapon_paints

