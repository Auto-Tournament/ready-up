#include "readyup/game_events.h"

#include "readyup/plugin_api.h"
#include "readyup/plugin_loader.h"

#include "readyup/config.h"
#include "readyup/command_buffer_hook.h"
#include "readyup/engine_surface.h"
#include "readyup/features.h"
#include "readyup/sdk/igameevents.h"
#include "readyup/logging.h"
#include "readyup/match_state.h"
#include "readyup/modes.h"
#include "readyup/backup_files.h"
#include "readyup/persisted_match_state.h"
#include "readyup/schema.h"
#include "readyup/signature_scan.h"
#include "readyup/slot_registry.h"
#include "readyup/real_server.h"
#include "readyup/webhook.h"
#include "readyup/postgres.h"
#include "readyup/weapon_paints.h"
#include "readyup/welcome.h"


#include "third_party/funchook/include/funchook.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace readyup {
static void InstallGameEventsListenerImpl(bool force);
namespace {

static long long NowMs() {
  using Clock = std::chrono::system_clock;
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

static std::optional<int> DefindexForDeathWeaponString(const char* weapon) {
  // `player_death` typically provides `weapon` as a short token like "ak47" (no "weapon_" prefix).
  if (!weapon || !*weapon) return std::nullopt;
  std::string w(weapon);
  if (w.rfind("weapon_", 0) == 0) w = w.substr(7);

  // Map based on cs2-WeaponPaints `Variables.cs` WeaponDefindex entries (subset).
  // Expand as needed; unknown weapons return nullopt.
  // Pistols
  if (w == "deagle") return 1;
  if (w == "elite") return 2;
  if (w == "fiveseven") return 3;
  if (w == "glock") return 4;
  if (w == "tec9") return 30;
  if (w == "hkp2000") return 32;
  if (w == "p250") return 36;
  if (w == "usp_silencer") return 61;
  if (w == "cz75a") return 63;
  if (w == "revolver") return 64;

  // Rifles/SMGs/Heavies
  if (w == "ak47") return 7;
  if (w == "aug") return 8;
  if (w == "awp") return 9;
  if (w == "famas") return 10;
  if (w == "g3sg1") return 11;
  if (w == "galilar") return 13;
  if (w == "m249") return 14;
  if (w == "m4a1") return 16;
  if (w == "mac10") return 17;
  if (w == "p90") return 19;
  if (w == "mp5sd") return 23;
  if (w == "ump45") return 24;
  if (w == "xm1014") return 25;
  if (w == "bizon") return 26;
  if (w == "mag7") return 27;
  if (w == "negev") return 28;
  if (w == "sawedoff") return 29;
  if (w == "taser") return 31;
  if (w == "mp7") return 33;
  if (w == "mp9") return 34;
  if (w == "nova") return 35;
  if (w == "scar20") return 38;
  if (w == "sg556") return 39;
  if (w == "ssg08") return 40;
  if (w == "m4a1_silencer") return 60;

  // Knives (often show as "knife", "knife_t", etc. on death events).
  if (w == "knife" || w == "knife_t" || w == "bayonet") return 500;  // treat as generic knife; cosmetic defindex is per-player loadout

  return std::nullopt;
}

// Game-event interface mirrors live in readyup/sdk/igameevents.h. They must keep external
// linkage: when they were declared in this anonymous namespace GCC devirtualized every call
// through them into a direct call to __cxa_pure_virtual ("pure virtual method called").
using sdk::CKV3MemberName;
using sdk::CPlayerSlot;
using sdk::IGameEvent;
using sdk::IGameEventListener2;
using sdk::IGameEventManager2;

std::atomic<bool> g_ok{false};
std::atomic<bool> g_eventsDelivered{false};
IGameEventManager2* g_mgr = nullptr;
std::atomic<long long> g_lastInstallAttemptMs{0};

// Observe interface pointers as they're created by the engine.
std::atomic<void*> g_source2Server{nullptr};
std::atomic<void*> g_source2ServerConfig{nullptr};
std::atomic<void*> g_gameEventSystem{nullptr};

// Listener registration state (AddListener path).
std::atomic<bool> g_listenerRegistered{false};

// Delivery stats for `ru selftest`. (The old FireEvent vtable-hook fallback -- guessed slot 8,
// probed IGameEvent vtables -- is gone; AddListener is the only registration path.)
std::atomic<unsigned long long> g_eventsCount{0};
std::atomic<int> g_eventLogLeft{30};
std::mutex g_lastEventMu;
std::string g_lastEventName;
std::atomic<bool> g_mgrRejected{false};


// --- Event manager acquisition ------------------------------------------------------
//
// CGameEventManager is a static object inside libserver.so. The server's init code does
//     lea rax, [rip + <gameeventmanager>]; mov rdi, [rax]; call CGameEventManager::Init
// (next to the "gameeventmanager->Init()" assert string). We locate that call site from the
// verified Init address, read the global pointer, and accept the object only if its vtable's
// RTTI typeinfo name is "17CGameEventManager". Hooking Init is only a fallback, and a
// captured `this` goes through the same RTTI check. Every later use re-checks that the
// object's vptr is still the verified one before making a virtual call.

std::atomic<void*> g_mgrVptr{nullptr};
std::atomic<bool> g_mgrVptrMismatchLogged{false};

static bool AcceptManager(void* cand, const char* via) {
  if (!cand) return false;
  if (!ObjectHasEngineRtti(cand, "CGameEventManager")) {
    g_mgrRejected.store(true);
    Print("game-events: rejected event manager candidate %p via %s (vtable RTTI is not CGameEventManager)\n", cand, via);
    return false;
  }
  void* vptr = nullptr;
  std::memcpy(&vptr, cand, sizeof(vptr));
  g_mgrVptr.store(vptr, std::memory_order_release);
  g_mgr = static_cast<IGameEventManager2*>(cand);
  Print("game-events: IGameEventManager2=%p via %s (RTTI CGameEventManager verified)\n", cand, via);
  return true;
}

// g_mgr, but only while its vptr is still the RTTI-verified CGameEventManager vtable.
static IGameEventManager2* VerifiedMgr() {
  IGameEventManager2* m = g_mgr;
  if (!m) return nullptr;
  void* cur = nullptr;
  std::memcpy(&cur, m, sizeof(cur));  // static object in libserver.so: always mapped
  if (cur != g_mgrVptr.load(std::memory_order_acquire)) {
    if (!g_mgrVptrMismatchLogged.exchange(true)) {
      Print("game-events: event manager %p vptr changed (%p); refusing virtual calls\n", (void*)m, cur);
    }
    return nullptr;
  }
  return m;
}

static bool ResolveManagerFromInitCallSite(uintptr_t init) {
  const es::Image* img = RealServerImage();
  if (!img || !init) return false;
  for (const auto& r : img->regions) {
    if (!r.exec || r.size < 16) continue;
    for (size_t i = 10; i + 5 <= r.size; ++i) {
      if (r.data[i] != 0xE8) continue;
      int32_t rel = 0;
      std::memcpy(&rel, r.data + i + 1, 4);
      if (r.addr + i + 5 + static_cast<intptr_t>(rel) != init) continue;
      // Expect: 48 8D 05 <disp32> (lea rax,[rip+disp]) ; 48 8B 38 (mov rdi,[rax]) ; E8 <Init>
      const uint8_t* p = r.data + i - 10;
      if (!(p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x05 && p[7] == 0x48 && p[8] == 0x8B && p[9] == 0x38)) continue;
      int32_t disp = 0;
      std::memcpy(&disp, p + 3, 4);
      const uintptr_t var = r.addr + (i - 10) + 7 + static_cast<intptr_t>(disp);
      const uint8_t* vb = img->Bytes(var, sizeof(void*));
      if (!vb) continue;
      void* cand = nullptr;
      std::memcpy(&cand, vb, sizeof(cand));
      if (AcceptManager(cand, "gameeventmanager global (Init call site)")) return true;
    }
  }
  return false;
}

// Fallback: capture `this` from CGameEventManager::Init (returns 1 in eax on 1.41.8.3).
using GameEventManagerInitFn = int (*)(void* mgr);
GameEventManagerInitFn g_geInit = nullptr;  // trampoline after funchook_prepare
funchook_t* g_geHook = nullptr;
std::atomic<bool> g_geResolveAttempted{false};

static int DetourGameEventManagerInit(void* mgr) {
  if (!g_mgr) (void)AcceptManager(mgr, "CGameEventManager::Init hook");
  return g_geInit ? g_geInit(mgr) : 0;
}

static void InstallInitHookFallback(void* init) {
  g_geInit = reinterpret_cast<GameEventManagerInitFn>(init);
  g_geHook = funchook_create();
  if (!g_geHook) {
    PrintLine("game-events: funchook_create failed for CGameEventManager_Init");
    return;
  }
  int rv = funchook_prepare(g_geHook, (void**)&g_geInit, (void*)&DetourGameEventManagerInit);
  if (rv == 0) rv = funchook_install(g_geHook, 0);
  if (rv != 0) {
    Print("game-events: CGameEventManager_Init hook failed (%d): %s\n", rv, funchook_error_message(g_geHook));
    funchook_destroy(g_geHook);
    g_geHook = nullptr;
    g_geInit = nullptr;
    return;
  }
  PrintLine("game-events: hooked CGameEventManager_Init (fallback; manager captured on next map load)");
}

static void EnsureGameEventManagerResolved() {
  if (g_mgr) return;
  if (g_geResolveAttempted.exchange(true)) return;

  // Unresolved => the "events" feature reports it (one line); nothing to do here.
  void* init = EngineFunction("CGameEventManager_Init");
  if (!init) return;
  if (ResolveManagerFromInitCallSite(reinterpret_cast<uintptr_t>(init))) return;
  InstallInitHookFallback(init);
}


struct PlayerStats {
  std::string name;
  WebhookTeam team = WebhookTeam::Unknown;
  int kills = 0;
  int deaths = 0;
  int assists = 0;
  int headshot_kills = 0;
  int damage = 0;
};

// Recursive: event handlers run with g_mu held and re-enter this module on the same thread
// (player_spawn -> GetCsTeamNumForSlot; round_end -> OnMatchRoundEnded ->
// GetRosterTeamDamageTotals). With a plain std::mutex those paths self-deadlock the game thread.
using EventsMutex = std::recursive_mutex;
EventsMutex g_mu;
int g_roundNumber = 0;
int g_lastMapNumber = 0;
std::unordered_map<uint64_t, PlayerStats> g_stats;
std::unordered_map<int, void*> g_slotController;

// Lifecycle tracking (per map).
int g_swapCount = 0;                  // number of side swaps applied so far (0 at map start)
int g_lastHalfStartTotal = -1;        // last (team1+team2) total rounds at which we emitted halftime+swap
int g_lastOvertimeNumber = 0;         // last overtime_number we emitted overtime_started for

static std::optional<uint64_t> SteamForSlot(int slot) {
  auto ident = GetSlotIdentity(slot);
  if (!ident || ident->steamid64 == 0) return std::nullopt;
  return ident->steamid64;
}

static std::string NameForSlot(int slot) {
  auto ident = GetSlotIdentity(slot);
  if (!ident) return {};
  return ident->name;
}

static WebhookTeam TeamForSteam(uint64_t steamid64) {
  auto ctx = WebhookGetMatchContext();
  if (!ctx) return WebhookTeam::Unknown;
  auto it = ctx->roster_team.find(steamid64);
  if (it == ctx->roster_team.end()) return WebhookTeam::Unknown;
  return it->second;
}

static void MaybeResetForMapLocked(int mapNumber) {
  if (mapNumber <= 0) mapNumber = 1;
  if (g_lastMapNumber != mapNumber) {
    g_lastMapNumber = mapNumber;
    g_roundNumber = 0;
    g_stats.clear();
    g_slotController.clear();
    g_swapCount = 0;
    g_lastHalfStartTotal = -1;
    g_lastOvertimeNumber = 0;
  }
}

template <typename T>
static std::optional<T> ReadAt(void* base, int offset) {
  if (!base) return std::nullopt;
  if (offset < 0 || offset > 0x20000) return std::nullopt;
  T out{};
  std::memcpy(&out, reinterpret_cast<const unsigned char*>(base) + offset, sizeof(T));
  return out;
}

// SteamID64 straight from an event's controller (CBasePlayerController::m_steamID),
// so an engine slot is turned into an identity without going through the
// log-keyed slot map. 0 for bots / unknown.
static uint64_t SteamFromController(void* controller) {
  if (!controller) return 0;
  static std::optional<int> s_off;
  static std::atomic<bool> s_lookedUp{false};
  bool expected = false;
  if (s_lookedUp.compare_exchange_strong(expected, true)) {
    s_off = SchemaFindOffset("server", "CCSPlayerController", "m_steamID");
    if (!s_off) PrintLine("game-events: m_steamID offset not found; event teams resolve via the log slot map.");
  }
  if (!s_off) return 0;
  const auto v = ReadAt<uint64_t>(controller, *s_off);
  if (!v || *v == 0) return 0;
  // Individual-account SteamID64s only (universe 1, type 1); anything else is not a human.
  if ((*v >> 52) != 0x011) return 0;
  return *v;
}

// SteamID64 for an event's `userid` player: the controller's m_steamID first, the
// log-keyed slot map as a fallback. Logs a debug line when both are known and differ.
static uint64_t ResolveEventSteam(IGameEvent* ev, int slot, const char* what) {
  uint64_t fromCtrl = SteamFromController(ev ? ev->GetPlayerController(CKV3MemberName("userid")) : nullptr);
  const uint64_t fromLog = SteamForSlot(slot).value_or(0);
  if (fromCtrl != 0 && fromLog != 0 && fromCtrl != fromLog) {
    Debug("teams: %s slot=%d controller steamid64=%llu but log slot map has %llu (using controller)\n", what, slot,
          static_cast<unsigned long long>(fromCtrl), static_cast<unsigned long long>(fromLog));
  }
  return fromCtrl != 0 ? fromCtrl : fromLog;
}

static std::optional<int> FindRosterSlot(uint64_t steamid64) {
  if (!steamid64) return std::nullopt;
  for (const auto& s : ListSlotIdentities()) {
    if (s.steamid64 == steamid64 && s.slot >= 0) return s.slot;
  }
  return std::nullopt;
}

static const char* WinnerToTeamString(int csWinnerTeamNum) {
  // CS team numbers: 2=T, 3=CT. We report MatchZy team1/team2 based on map_sides when present.
  auto ctx = WebhookGetMatchContext();
  const auto ms = MatchStateGet();
  bool team1IsCt = true;
  if (ctx) {
    if (ms.map_number >= 1 && static_cast<size_t>(ms.map_number) <= ctx->map_sides.size()) {
      const std::string& side = ctx->map_sides[static_cast<size_t>(ms.map_number - 1)];
      if (side == "team2_ct") team1IsCt = false;
      else if (side == "team1_ct") team1IsCt = true;
    }
  }

  if (csWinnerTeamNum == 3) {  // CT won
    return team1IsCt ? "team1" : "team2";
  }
  if (csWinnerTeamNum == 2) {  // T won
    return team1IsCt ? "team2" : "team1";
  }
  return "team1";
}

static std::optional<int> GetEventIntIfPresent(IGameEvent* ev, const char* key) {
  if (!ev || !key || !*key) return std::nullopt;
  CKV3MemberName k(key);
  if (ev->IsEmpty(k)) return std::nullopt;
  return ev->GetInt(k, 0);
}

static std::optional<int> ReadScoreFromEvent(IGameEvent* ev, const char* const* keys, size_t keyCount) {
  for (size_t i = 0; i < keyCount; ++i) {
    if (auto v = GetEventIntIfPresent(ev, keys[i])) return v;
  }
  return std::nullopt;
}

static bool InitialTeam1IsCtForMap(int mapNumber) {
  bool team1IsCt = true;
  auto ctx = WebhookGetMatchContext();
  if (ctx) {
    if (mapNumber >= 1 && static_cast<size_t>(mapNumber) <= ctx->map_sides.size()) {
      const std::string& side = ctx->map_sides[static_cast<size_t>(mapNumber - 1)];
      if (side == "team2_ct") team1IsCt = false;
      else if (side == "team1_ct") team1IsCt = true;
    }
  }
  return team1IsCt;
}

static bool Team1IsCtWithSwapCount(int mapNumber, int swapCount) {
  bool team1IsCt = InitialTeam1IsCtForMap(mapNumber);
  if ((swapCount % 2) != 0) team1IsCt = !team1IsCt;
  return team1IsCt;
}

static void EmitRoundEndLocked(int csWinnerTeamNum, int reason) {
  auto ctxOpt = WebhookGetMatchContext();
  if (!ctxOpt) return;
  const auto& ctx = *ctxOpt;
  const auto ms = MatchStateGet();

  auto bestNameForSteam = [&](uint64_t steamid64) -> std::string {
    if (steamid64 == 0) return {};
    // Prefer last observed via events.
    auto it = g_stats.find(steamid64);
    if (it != g_stats.end() && !it->second.name.empty()) return it->second.name;
    // Fall back to slot identity observations.
    for (const auto& s : ListSlotIdentities()) {
      if (s.steamid64 == steamid64 && !s.name.empty()) return s.name;
    }
    return {};
  };

  std::vector<WebhookPlayerStats> team1;
  std::vector<WebhookPlayerStats> team2;
  team1.reserve(ctx.roster_team.size());
  team2.reserve(ctx.roster_team.size());

  // Netvar offsets (best-effort). If these fail, we keep event-accumulated stats.
  const auto offKills = SchemaFindOffset("server", "CCSPlayerController", "m_iKills");
  const auto offDeaths = SchemaFindOffset("server", "CCSPlayerController", "m_iDeaths");
  const auto offAssists = SchemaFindOffset("server", "CCSPlayerController", "m_iAssists");
  const auto offMvps = SchemaFindOffset("server", "CCSPlayerController", "m_iMVPs");
  const auto offScore = SchemaFindOffset("server", "CCSPlayerController", "m_iScore");
  auto findFirst = [&](std::initializer_list<const char*> names) -> std::optional<int> {
    for (const char* n : names) {
      if (!n || !*n) continue;
      if (auto off = SchemaFindOffset("server", "CCSPlayerController", n)) return off;
    }
    return std::nullopt;
  };
  const auto offHeadshots = findFirst({"m_iHeadshotKills", "m_iHeadShotKills", "m_iMatchStats_HeadshotKills"});

  for (const auto& kv : ctx.roster_team) {
    const uint64_t sid = kv.first;
    const WebhookTeam team = kv.second;
    PlayerStats st{};
    if (auto it = g_stats.find(sid); it != g_stats.end()) st = it->second;
    if (st.team == WebhookTeam::Unknown) st.team = team;
    if (st.name.empty()) st.name = bestNameForSteam(sid);

    // Try netvar snapshot from controller if we can resolve it.
    if (auto slotOpt = FindRosterSlot(sid)) {
      auto itc = g_slotController.find(*slotOpt);
      if (itc != g_slotController.end() && itc->second) {
        void* ctrl = itc->second;
        if (offKills) {
          if (auto v = ReadAt<int>(ctrl, *offKills)) st.kills = *v;
        }
        if (offDeaths) {
          if (auto v = ReadAt<int>(ctrl, *offDeaths)) st.deaths = *v;
        }
        if (offAssists) {
          if (auto v = ReadAt<int>(ctrl, *offAssists)) st.assists = *v;
        }
        if (offMvps) {
          if (auto v = ReadAt<int>(ctrl, *offMvps)) {
            if (*v >= 0 && *v < 1000) {
              // store in a temp field via g_stats? we keep it local below.
            }
          }
        }
        if (offScore) {
          if (auto v = ReadAt<int>(ctrl, *offScore)) {
            if (*v >= 0 && *v < 1000000) {
              // store in a temp field via g_stats? we keep it local below.
            }
          }
        }
        if (offHeadshots) {
          if (auto v = ReadAt<int>(ctrl, *offHeadshots)) {
            // Only trust if plausible.
            if (*v >= 0 && *v <= st.kills) {
              st.headshot_kills = std::max(st.headshot_kills, *v);
            }
          }
        }
      }
    }

    WebhookPlayerStats out;
    out.steamid64 = sid;
    out.name = st.name.empty() ? std::to_string(sid) : st.name;
    out.kills = st.kills;
    out.deaths = st.deaths;
    out.assists = st.assists;
    out.headshot_kills = st.headshot_kills;
    out.damage = st.damage;
    // Score/MVPs are optional; read directly from controller when possible.
    // If unavailable, they remain 0.
    if (auto slotOpt = FindRosterSlot(sid)) {
      auto itc = g_slotController.find(*slotOpt);
      if (itc != g_slotController.end() && itc->second) {
        void* ctrl = itc->second;
        if (offMvps) {
          if (auto v = ReadAt<int>(ctrl, *offMvps)) {
            if (*v >= 0 && *v < 1000) out.mvps = *v;
          }
        }
        if (offScore) {
          if (auto v = ReadAt<int>(ctrl, *offScore)) {
            if (*v >= 0 && *v < 1000000) out.score = *v;
          }
        }
      }
    }

    if (team == WebhookTeam::Team1) team1.push_back(out);
    else if (team == WebhookTeam::Team2) team2.push_back(out);
  }

  WebhookEmitRoundEndMatchzy(
      ms.map_number,
      ms.round_number > 0 ? ms.round_number : g_roundNumber,
      /*round_time=*/0,
      reason,
      WinnerToTeamString(csWinnerTeamNum),
      ms.team1_score,
      ms.team2_score,
      team1,
      team2);

  // Persist a minimal snapshot for crash/restart recovery.
  readyup::persisted_match_state::PersistSnapshot(
      ms.map_number <= 0 ? 1 : ms.map_number,
      ms.round_number > 0 ? ms.round_number : g_roundNumber,
      ms.team1_score,
      ms.team2_score);

  // Best-effort: update backup prefix for current map and discover latest backup file.
  {
    const int mapNumber = ms.map_number <= 0 ? 1 : ms.map_number;
    const std::string prefix =
        "readyup_backup_" + std::to_string(static_cast<unsigned long long>(ctx.matchid)) +
        "_map" + std::to_string(mapNumber) + "_";
    readyup::persisted_match_state::PersistBackupPrefix(prefix);
    const std::string cmd = "mp_backup_round_file " + prefix;
    (void)EnqueueServerCommand(cmd.c_str());
    readyup::backup_files::DiscoverAndPersistNewestBackupFileAsync(prefix);
  }

  // Detect end-of-map (demo stop + map_result) once scores are known.
  OnMatchRoundEnded(ms.map_number, ms.team1_score, ms.team2_score, ms.current_map);
}

static void OnRoundStartLocked(IGameEvent* ev) {
  const auto ms = MatchStateGet();
  MaybeResetForMapLocked(ms.map_number);
  const int mapNumber = ms.map_number <= 0 ? 1 : ms.map_number;

  // Knife decider: do not emit match rounds or advance counters.
  if (GetMode() == ReadyUpMode::MatchKnife) {
    KnifeOnRoundStart(mapNumber, "event");
    return;
  }

  // Best-effort score snapshot. Prefer engine event keys when present.
  int team1Score = ms.team1_score;
  int team2Score = ms.team2_score;
  {
    static const char* const kCtKeys[] = {"ct_score", "score_ct", "ct_score_total"};
    static const char* const kTKeys[] = {"t_score", "score_t", "t_score_total"};
    const auto ct = ReadScoreFromEvent(ev, kCtKeys, sizeof(kCtKeys) / sizeof(kCtKeys[0]));
    const auto tt = ReadScoreFromEvent(ev, kTKeys, sizeof(kTKeys) / sizeof(kTKeys[0]));
    if (ct && tt) {
      // Use the side mapping for *this round*. If we decide a swap happens at this boundary
      // below, we'll apply it before mapping ct/t -> team1/team2.
      // (We map again below once we finalize swapCountForThisRound.)
    }
  }

  auto ctxOpt = WebhookGetMatchContext();
  if (ctxOpt) {
    const auto& ctx = *ctxOpt;
    const int maxRounds = std::max(1, ctx.maxRounds);
    const bool otEnabled = ctx.overtime_enabled;
    const int seg = std::max(1, ctx.overtimeSegments);
    const int sum = team1Score + team2Score;

    bool emitHalfStart = false;
    bool emitOvertimeStart = false;
    int overtimeNumber = 0;

    // Regulation halftime boundary.
    const int regHalf = maxRounds / 2;
    if (sum == regHalf && g_lastHalfStartTotal != sum) {
      emitHalfStart = true;
    }

    // Overtime boundaries.
    if (otEnabled && sum >= maxRounds && team1Score == team2Score) {
      const int roundsPastReg = sum - maxRounds;
      const int blockSize = 2 * seg;
      if (blockSize > 0) {
        const int block = roundsPastReg / blockSize;  // 0-based OT index
        const int offset = roundsPastReg % blockSize;
        overtimeNumber = block + 1;

        if (offset == 0) {
          // Start of OT block (OT1/OT2/...) after a tied segment.
          if (overtimeNumber > g_lastOvertimeNumber) {
            emitOvertimeStart = true;
            g_lastOvertimeNumber = overtimeNumber;
          }
          // Treat OT start as a half start (swap + halftime_started) too.
          if (g_lastHalfStartTotal != sum) {
            emitHalfStart = true;
          }
        } else if (offset == seg) {
          // Start of OT half 2.
          if (g_lastHalfStartTotal != sum) {
            emitHalfStart = true;
          }
        }
      }
    }

    if (emitOvertimeStart && overtimeNumber > 0) {
      WebhookEmitOvertimeStarted(mapNumber, overtimeNumber);
    }

    // Every half start implies a side swap (except the very first half, which does not emit).
    if (emitHalfStart) {
      g_lastHalfStartTotal = sum;
      const int swapCountForThisRound = g_swapCount + 1;
      const bool team1IsCtNow = Team1IsCtWithSwapCount(mapNumber, swapCountForThisRound);
      const char* t1Side = team1IsCtNow ? "CT" : "T";
      const char* t2Side = team1IsCtNow ? "T" : "CT";

      WebhookEmitHalftimeStarted(mapNumber, team1Score, team2Score);
      WebhookEmitSideSwap(mapNumber, t1Side, t2Side);

      g_swapCount = swapCountForThisRound;
    }

    // If we have event-provided ct/t scores, map them now using the current swap count
    // (after applying any boundary swap above).
    static const char* const kCtKeys[] = {"ct_score", "score_ct", "ct_score_total"};
    static const char* const kTKeys[] = {"t_score", "score_t", "t_score_total"};
    const auto ct = ReadScoreFromEvent(ev, kCtKeys, sizeof(kCtKeys) / sizeof(kCtKeys[0]));
    const auto tt = ReadScoreFromEvent(ev, kTKeys, sizeof(kTKeys) / sizeof(kTKeys[0]));
    if (ct && tt) {
      const bool team1IsCtNow = Team1IsCtWithSwapCount(mapNumber, g_swapCount);
      const int ctScore = std::max(0, *ct);
      const int tScore = std::max(0, *tt);
      team1Score = team1IsCtNow ? ctScore : tScore;
      team2Score = team1IsCtNow ? tScore : ctScore;
    }
  }

  g_roundNumber += 1;
  MatchStateSetRound(g_roundNumber);

  // Warmup -> live transition (first real round start).
  if (GetMode() == ReadyUpMode::MatchWarmup) {
    OnMatchRoundStarted();
  }
  WebhookEmitRoundStarted(mapNumber, g_roundNumber, team1Score, team2Score);
}

static void OnPlayerDeathLocked(IGameEvent* ev) {
  if (!ev) return;
  const int attackerSlot = ev->GetPlayerSlot(CKV3MemberName("attacker")).value;
  const int victimSlot = ev->GetPlayerSlot(CKV3MemberName("userid")).value;
  const int assisterSlot = ev->GetPlayerSlot(CKV3MemberName("assister")).value;
  const bool headshot = ev->GetBool(CKV3MemberName("headshot"), false);

  const auto attackerSid = SteamForSlot(attackerSlot);
  const auto victimSid = SteamForSlot(victimSlot);
  const auto assisterSid = SteamForSlot(assisterSlot);

  if (attackerSid && *attackerSid != 0) {
    auto& s = g_stats[*attackerSid];
    const std::string nm = NameForSlot(attackerSlot);
    if (!nm.empty()) s.name = nm;
    s.team = TeamForSteam(*attackerSid);
    s.kills += 1;
    if (headshot) s.headshot_kills += 1;
  }

  if (victimSid && *victimSid != 0) {
    auto& s = g_stats[*victimSid];
    const std::string nm = NameForSlot(victimSlot);
    if (!nm.empty()) s.name = nm;
    s.team = TeamForSteam(*victimSid);
    s.deaths += 1;
  }

  if (assisterSid && attackerSid && victimSid) {
    if (*assisterSid != 0 && *assisterSid != *attackerSid && *assisterSid != *victimSid) {
      auto& s = g_stats[*assisterSid];
      const std::string nm = NameForSlot(assisterSlot);
      if (!nm.empty()) s.name = nm;
      s.team = TeamForSteam(*assisterSid);
      s.assists += 1;
    }
  }

  // Best-effort StatTrak increment (DB-backed).
  // We rely on the death event's `weapon` token (e.g. "ak47") to map to defindex.
  if (attackerSid && *attackerSid != 0) {
    const char* w = ev->GetString(CKV3MemberName("weapon"), "");
    const auto defOpt = DefindexForDeathWeaponString(w);
    const int attackerTeamNum = ev->GetInt(CKV3MemberName("attackerteam"), 0);  // 2=T, 3=CT
    if (defOpt && attackerTeamNum != 0) {
      const int defindex = *defOpt;
      // Only increment when the player's configured skin row has StatTrak enabled.
      const auto skin = readyup::weapon_paints::FindWeaponSkin(*attackerSid, attackerTeamNum, defindex);
      if (skin && skin->stattrak_enabled) {
        // Increment the row that actually matched (may be the team-0 "both" row).
        std::thread([sid = *attackerSid, team = skin->weapon_team, def = defindex]() {
          std::string err;
          if (readyup::pg::IncrementWeaponSkinStatTrakCount(sid, team, def, &err)) {
            // Drop cache so next apply sees the incremented count.
            readyup::weapon_paints::Invalidate(sid);
          } else if (readyup::DebugEnabled() && !err.empty()) {
            readyup::Debug("weapon_paints: IncrementWeaponSkinStatTrakCount(%llu,%d,%d) err=%s\n",
                           static_cast<unsigned long long>(sid), team, def, err.c_str());
          }
        }).detach();
      }
    }
  }
}

static void OnPlayerHurtLocked(IGameEvent* ev) {
  if (!ev) return;
  const int attackerSlot = ev->GetPlayerSlot(CKV3MemberName("attacker")).value;
  const int dmg = ev->GetInt(CKV3MemberName("dmg_health"), 0);
  const auto attackerSid = SteamForSlot(attackerSlot);
  if (!attackerSid || *attackerSid == 0) return;
  auto& s = g_stats[*attackerSid];
  const std::string nm = NameForSlot(attackerSlot);
  if (!nm.empty()) s.name = nm;
  s.team = TeamForSteam(*attackerSid);
  s.damage += std::max(0, dmg);
}

static void OnPlayerSpawnLocked(IGameEvent* ev) {
  if (!ev) return;
  const int slot = ev->GetPlayerSlot(CKV3MemberName("userid")).value;
  void* controller = ev->GetPlayerController(CKV3MemberName("userid"));
  if (slot >= 0 && controller) {
    g_slotController[slot] = controller;
    if (const uint64_t sid = SteamFromController(controller)) ObserveHumanSlot(sid, slot);
  }

  // Kick off async loadout refresh. Actual apply is best-effort and may happen
  // later via weapon hooks (or on future spawn when offsets/hooks are available).
  if (auto sid = SteamForSlot(slot)) {
    readyup::weapon_paints::MaybeRefreshAsync(*sid);
    void* pawn = ev->GetPlayerPawn(CKV3MemberName("userid"));
    int teamNum = ev->GetInt(CKV3MemberName("team"), 0);
    if (teamNum == 0) {
      if (auto t = GetCsTeamNumForSlot(slot)) teamNum = *t;
    }
    // Gloves/agents are applied by weapon_paints::GameFrameTick() on the spawn frame.
    (void)pawn;
  }
}

static void OnItemEquipLocked(IGameEvent* ev) {
  if (!ev) return;
  const int slot = ev->GetPlayerSlot(CKV3MemberName("userid")).value;
  const auto sid = SteamForSlot(slot);
  if (!sid || *sid == 0) return;
  // Prefetch only. Skins are applied by weapon_paints::GameFrameTick(), which sees the new weapon
  // entity in the pawn's m_hMyWeapons (item_equip's "item" key is a classname string, not an entity).
  readyup::weapon_paints::MaybeRefreshAsync(*sid);
}

static void OnItemPickupLocked(IGameEvent* ev) {
  if (!ev) return;
  const int slot = ev->GetPlayerSlot(CKV3MemberName("userid")).value;
  const auto sid = SteamForSlot(slot);
  if (!sid || *sid == 0) return;
  readyup::weapon_paints::MaybeRefreshAsync(*sid);
}

// Normalized lifecycle events for plugins (engine source). Queued; delivered on GameFrame.
static void PostPluginEvent(uint32_t type, IGameEvent* ev, int round) {
  plugins::LifecycleEvent e;
  e.type = type;
  e.source = RU_SOURCE_ENGINE;
  e.round = round;
  if (type == RU_EVENT_ROUND_END) {
    e.winner = ev->GetInt(CKV3MemberName("winner"), 0);
    e.reason = ev->GetInt(CKV3MemberName("reason"), 0);
  } else if (type != RU_EVENT_ROUND_START) {
    e.slot = ev->GetPlayerSlot(CKV3MemberName("userid")).value;
    e.steamid64 = SteamForSlot(e.slot).value_or(0);
    e.name = NameForSlot(e.slot);
    if (type == RU_EVENT_PLAYER_DISCONNECT) {
      const uint64_t xuid = ev->GetUint64(CKV3MemberName("xuid"), 0);
      if (xuid != 0) e.steamid64 = xuid;
      const char* n = ev->GetString(CKV3MemberName("name"), "");
      if (n && *n) e.name = n;
      e.reason = ev->GetInt(CKV3MemberName("reason"), 0);
    } else if (type == RU_EVENT_PLAYER_TEAM) {
      e.team = ev->GetInt(CKV3MemberName("team"), 0);
      e.old_team = ev->GetInt(CKV3MemberName("oldteam"), 0);
    }
  }
  plugins::PostEvent(std::move(e));
}

static void PostPluginEventFor(const char* name, IGameEvent* ev) {
  if (std::strcmp(name, "player_connect_full") == 0) {
    PostPluginEvent(RU_EVENT_PLAYER_CONNECT, ev, 0);
  } else if (std::strcmp(name, "player_disconnect") == 0) {
    PostPluginEvent(RU_EVENT_PLAYER_DISCONNECT, ev, 0);
  } else if (std::strcmp(name, "player_team") == 0) {
    if (ev->GetInt(CKV3MemberName("disconnect"), 0) == 0) PostPluginEvent(RU_EVENT_PLAYER_TEAM, ev, 0);
  }
}

struct ListenerImpl : IGameEventListener2 {
  void FireGameEvent(IGameEvent* event) override {
    g_eventsCount.fetch_add(1, std::memory_order_relaxed);
    if (!event) return;
    const char* name = event->GetName();
    if (!name || !*name) return;
    {
      std::lock_guard<std::mutex> lk(g_lastEventMu);
      g_lastEventName = name;
    }
    if (!g_eventsDelivered.exchange(true, std::memory_order_acq_rel)) {
      Print("game-events: first engine event delivered (\"%s\"); engine events now drive the round lifecycle.\n",
            name);
    }

    if (DebugEnabled()) {
      int left = g_eventLogLeft.load(std::memory_order_relaxed);
      while (left > 0 && !g_eventLogLeft.compare_exchange_weak(left, left - 1)) {
        // retry
      }
      if (left > 0) {
        Debug("game-events: event name=\"%s\" event=%p\n", name, (void*)event);
      }
    }

    PostPluginEventFor(name, event);
    // Raw events for plugins (subscribe_game_event): synchronous, before any core lock is taken.
    plugins::DispatchGameEvent(name, event);

    // Clear ready state on disconnect before taking g_mu (ClearReady takes the modes mutex).
    if (std::strcmp(name, "player_disconnect") == 0) {
      const uint64_t xuid = event->GetUint64(CKV3MemberName("xuid"), 0);
      if (xuid != 0) ClearReady(xuid);
    }
    // CS2 native warmup (re)started: Ready Up emulates warmup, end the real one
    // (takes the modes mutex, so before g_mu).
    if (std::strcmp(name, "round_announce_warmup") == 0) {
      OnNativeWarmupStarted("round_announce_warmup event");
      return;
    }

    const auto ms = MatchStateGet();
    std::lock_guard<EventsMutex> lk(g_mu);
    MaybeResetForMapLocked(ms.map_number);

    // Lightweight debug logs for common lifecycle events.
    if (DebugEnabled()) {
      if (std::strcmp(name, "player_connect") == 0 || std::strcmp(name, "player_connect_full") == 0) {
        const char* pname = event->GetString(CKV3MemberName("name"), "");
        const uint64_t xuid = event->GetUint64(CKV3MemberName("xuid"), 0);
        const int slot = event->GetPlayerSlot(CKV3MemberName("userid")).value;
        Debug("game-events: %s name=\"%s\" xuid=%llu slot=%d\n", name, pname ? pname : "", (unsigned long long)xuid, slot);
      } else if (std::strcmp(name, "player_disconnect") == 0) {
        const char* pname = event->GetString(CKV3MemberName("name"), "");
        const int reason = event->GetInt(CKV3MemberName("reason"), 0);
        Debug("game-events: player_disconnect name=\"%s\" reason=%d\n", pname ? pname : "", reason);
      } else if (std::strcmp(name, "player_team") == 0) {
        const int team = event->GetInt(CKV3MemberName("team"), 0);
        const int oldteam = event->GetInt(CKV3MemberName("oldteam"), 0);
        const int disc = event->GetInt(CKV3MemberName("disconnect"), 0);
        const int slot = event->GetPlayerSlot(CKV3MemberName("userid")).value;
        Debug("game-events: player_team slot=%d old=%d new=%d disconnect=%d\n", slot, oldteam, team, disc);
      } else if (std::strcmp(name, "player_spawn") == 0) {
        const int slot = event->GetPlayerSlot(CKV3MemberName("userid")).value;
        const int team = event->GetInt(CKV3MemberName("team"), 0);
        Debug("game-events: player_spawn slot=%d team=%d\n", slot, team);
      } else if (std::strcmp(name, "weapon_reload") == 0) {
        const char* w = event->GetString(CKV3MemberName("weapon"), "");
        const int slot = event->GetPlayerSlot(CKV3MemberName("userid")).value;
        Debug("game-events: weapon_reload slot=%d weapon=\"%s\"\n", slot, w ? w : "");
      }
    }

    if (std::strcmp(name, "player_disconnect") == 0) {
      const int slot = event->GetPlayerSlot(CKV3MemberName("userid")).value;
      if (slot >= 0) g_slotController.erase(slot);
      return;
    }
    if (std::strcmp(name, "player_team") == 0) {
      const int team = event->GetInt(CKV3MemberName("team"), 0);
      // GetInt (not GetBool): same accessor the debug path above already uses.
      const bool disc = event->GetInt(CKV3MemberName("disconnect"), 0) != 0;
      const bool isBot = event->GetInt(CKV3MemberName("isbot"), 0) != 0;
      if (!disc && !isBot) {
        const int slot = event->GetPlayerSlot(CKV3MemberName("userid")).value;
        const uint64_t sid = ResolveEventSteam(event, slot, "player_team");
        // Team source #2 for the SteamID-keyed human table (log lines are #1).
        if (sid != 0 && team >= 0 && team <= 3) {
          const bool known = ObserveHumanTeamFromEvent(sid, slot, team);
          Debug("teams: player_team steamid64=%llu slot=%d team=%d%s\n", static_cast<unsigned long long>(sid), slot,
                team, known ? "" : " (not a connected human yet; ignored)");
        }
        // Welcome screen trigger (one of several sources; deduped in welcome.cpp).
        if (team == 2 || team == 3) {
          WelcomeObserveTeamJoin(slot, team, sid, NameForSlot(slot), WelcomeSource::GameEvent);
        }
      }
      return;
    }
    if (std::strcmp(name, "round_start") == 0) {
      OnRoundStartLocked(event);
      PostPluginEvent(RU_EVENT_ROUND_START, event, g_roundNumber);
      return;
    }
    if (std::strcmp(name, "round_end") == 0) {
      PostPluginEvent(RU_EVENT_ROUND_END, event, g_roundNumber);
      const int winner = event->GetInt(CKV3MemberName("winner"), 0);
      const int reason = event->GetInt(CKV3MemberName("reason"), 0);
      if (GetMode() == ReadyUpMode::MatchKnife) {
        // Only eliminations are decided from the event (CS2 reasons: 8 = CTs win,
        // 9 = Terrorists win). Time-outs/draws need the alive/HP tiebreak, which the
        // log path (SFUI_Notice_* line) runs; it also covers eliminations when
        // events are down. The first source wins, the other is ignored by phase.
        if (reason == 8 || reason == 9) {
          KnifeOnRoundEnd(ms.map_number <= 0 ? 1 : ms.map_number, winner, /*elimination=*/true, "event",
                          reason == 8 ? "reason=8" : "reason=9");
        }
        return;
      }
      EmitRoundEndLocked(winner, reason);
      return;
    }
    if (std::strcmp(name, "player_death") == 0) {
      OnPlayerDeathLocked(event);
      return;
    }
    if (std::strcmp(name, "player_hurt") == 0) {
      OnPlayerHurtLocked(event);
      return;
    }
    if (std::strcmp(name, "player_spawn") == 0) {
      OnPlayerSpawnLocked(event);
      return;
    }
    if (std::strcmp(name, "item_equip") == 0) {
      OnItemEquipLocked(event);
      return;
    }
    if (std::strcmp(name, "item_pickup") == 0) {
      OnItemPickupLocked(event);
      return;
    }
  }
};

ListenerImpl g_listener;


}  // namespace

std::pair<int, int> GetRosterTeamDamageTotals() {
  std::lock_guard<EventsMutex> lk(g_mu);
  auto ctxOpt = WebhookGetMatchContext();
  if (!ctxOpt) return {0, 0};
  const auto& ctx = *ctxOpt;

  int team1 = 0;
  int team2 = 0;
  for (const auto& kv : ctx.roster_team) {
    const uint64_t sid = kv.first;
    const WebhookTeam team = kv.second;
    auto it = g_stats.find(sid);
    if (it == g_stats.end()) continue;
    const int dmg = std::max(0, it->second.damage);
    if (team == WebhookTeam::Team1) team1 += dmg;
    else if (team == WebhookTeam::Team2) team2 += dmg;
  }
  return {team1, team2};
}


// Events ListenerImpl consumes. `core` events must all be registered before we report
// "installed"; the others are best-effort.
struct ListenedEvent {
  const char* name;
  bool core;
};
static constexpr ListenedEvent kListenedEvents[] = {
    {"round_start", true},
    {"round_end", true},
    {"player_death", true},
    {"player_hurt", true},
    {"player_spawn", true},
    {"item_equip", true},
    {"item_pickup", true},
    {"player_team", false},
    {"player_connect_full", false},
    {"player_disconnect", false},
    {"round_announce_warmup", false},
};

// Probe used to tell (a) whether the manager has loaded its descriptors and (b) whether our
// registration survived. Must be a core event.
static constexpr const char* kProbeEvent = "round_start";

// Registers g_listener for kListenedEvents. Engine calls only; must run on the game thread.
//
// Why this is gated (CS2 1.41.8.3, verified in libserver.so):
//  - Our constructor runs before CServerGameDLL init calls CGameEventManager::Init, which does
//    Reset() (clears every descriptor's listener list) and then LoadEventsFromFile().
//  - Before that, AddListener() finds no descriptor, DevMsg's "AddListener: event '%s' unknown."
//    and returns -1. As a bool that reads as success, so the old code declared itself
//    installed and never registered again: FireGameEvent was never called.
// So: wait until descriptors exist, ignore AddListener's return value, and treat an event as
// registered only when FindListener() confirms it.
static bool RegisterListenersBestEffort() {
  IGameEventManager2* mgr = VerifiedMgr();
  if (!mgr) return false;

  static std::atomic<bool> s_waitLogged{false};
  if (!mgr->HasEventDescriptor(kProbeEvent)) {
    if (!s_waitLogged.exchange(true)) {
      PrintLine("game-events: event descriptors not loaded yet; waiting to register listeners.");
    }
    return false;
  }

  int verified = 0;
  int wanted = 0;
  bool allCoreOk = true;
  std::string missing;
  for (const auto& e : kListenedEvents) {
    ++wanted;
    if (!mgr->HasEventDescriptor(e.name)) {
      if (e.core) allCoreOk = false;
      missing += missing.empty() ? "" : ",";
      missing += e.name;
      missing += "(no descriptor)";
      continue;
    }
    if (!mgr->FindListener(&g_listener, e.name)) {
      (void)mgr->AddListener(&g_listener, e.name, /*bServerSide=*/true);  // return value is unreliable
    }
    if (mgr->FindListener(&g_listener, e.name)) {
      ++verified;
      continue;
    }
    if (e.core) allCoreOk = false;
    missing += missing.empty() ? "" : ",";
    missing += e.name;
  }

  if (!allCoreOk) {
    static std::atomic<int> s_failLogs{3};
    if (s_failLogs.fetch_sub(1) > 0) {
      Print("game-events: AddListener not confirmed for core events [%s]; will retry.\n", missing.c_str());
    }
    return false;
  }
  g_listenerRegistered.store(true, std::memory_order_release);
  Print("game-events: installed (AddListener; %d/%d events confirmed by FindListener%s%s).\n", verified, wanted,
        missing.empty() ? "" : "; skipped: ", missing.c_str());
  return true;
}

// Detects a registration that was wiped (e.g. the manager was Reset/re-Init'ed after we
// registered). Returns false if we must register again.
static bool ListenerStillRegistered(IGameEventManager2* mgr) {
  return mgr && mgr->FindListener(&g_listener, kProbeEvent);
}

static void InstallGameEventsListenerImpl(bool force) {
  // Engine calls happen here, so this must only run on the game thread (GameFrame -> Tick).
  const long long now = NowMs();
  const long long last = g_lastInstallAttemptMs.load(std::memory_order_relaxed);
  if (!force && last != 0 && (now - last) < 2000) return;
  g_lastInstallAttemptMs.store(now, std::memory_order_relaxed);

  // Needs a verified CGameEventManager_Init and an RTTI-verified manager (located on this call
  // or captured by the Init hook on map load); otherwise the "events" feature logs its one
  // "disabled" line and we never touch the event system.
  if (!FeatureEnabled(Feature::Events)) return;

  IGameEventManager2* mgr = VerifiedMgr();
  if (!mgr) {
    if (DebugEnabled()) {
      PrintLine("game-events: IGameEventManager2 unavailable (waiting for capture/resolve); engine events disabled.");
    }
    return;
  }

  if (g_ok.load(std::memory_order_acquire)) {
    if (ListenerStillRegistered(mgr)) return;
    PrintLine("game-events: listener registration lost (event manager reset?); re-registering.");
    g_ok.store(false, std::memory_order_release);
    g_listenerRegistered.store(false, std::memory_order_release);
    g_eventsDelivered.store(false, std::memory_order_release);  // log fallback until events flow again
  }

  // Register listeners through the HL2SDK interface (the only mode: the old FireEvent vtable
  // hook fallback guessed slot 8 and probed IGameEvent vtables, and is gone).
  if (RegisterListenersBestEffort()) g_ok.store(true, std::memory_order_release);
}

// Off the game thread (constructor, CreateInterface wrapper): only locate and RTTI-verify the
// manager. No virtual calls into it here; those happen in GameEventsFrameTick().
void InstallGameEventsListener() {
  if (g_ok.load(std::memory_order_acquire)) return;
  EnsureGameEventManagerResolved();
}

int GameEventManagerStatus(std::string* detail) {
  auto set = [&](const char* d) {
    if (detail) *detail = d;
  };
  if (!RealServerImage()) {
    set("real libserver.so not loaded");
    return 0;
  }
  if (!EngineFunctionResolution("CGameEventManager_Init").ok) {
    set("CGameEventManager_Init unresolved");
    return 2;
  }
  if (GameEventManagerVerified()) {
    set("CGameEventManager located, RTTI verified");
    return 1;
  }
  if (g_mgrRejected.load()) {
    set("event manager candidate failed the CGameEventManager RTTI check");
    return 2;
  }
  if (g_mgrVptr.load(std::memory_order_acquire)) {
    set("event manager vptr changed after verification");
    return 2;
  }
  set("waiting for CGameEventManager::Init (next map load)");
  return 0;
}

GameEventsStatus GetGameEventsStatus() {
  GameEventsStatus s;
  s.manager = VerifiedMgr() != nullptr;
  s.listenerRegistered = g_listenerRegistered.load(std::memory_order_acquire);
  s.delivered = g_eventsDelivered.load(std::memory_order_acquire);
  s.count = g_eventsCount.load(std::memory_order_relaxed);
  std::lock_guard<std::mutex> lk(g_lastEventMu);
  s.lastEvent = g_lastEventName;
  return s;
}

// Adds the listener for event names plugins subscribed to (subscribe_game_event). Game thread.
// Re-checked when the wanted set changes and every ~2 s (a manager Reset drops listeners).
static void RegisterPluginGameEvents() {
  static uint64_t s_gen = ~0ull;
  static long long s_lastMs = 0;
  const uint64_t gen = plugins::WantedGameEventsGeneration();
  const long long now = NowMs();
  if (gen == s_gen && now - s_lastMs < 2000) return;
  s_lastMs = now;
  IGameEventManager2* mgr = VerifiedMgr();
  if (!mgr || !g_ok.load(std::memory_order_acquire)) return;
  s_gen = gen;
  for (const auto& name : plugins::WantedGameEvents()) {
    if (!mgr->HasEventDescriptor(name.c_str())) continue;  // unknown to this build: never delivered
    if (!mgr->FindListener(&g_listener, name.c_str())) (void)mgr->AddListener(&g_listener, name.c_str(), true);
  }
}

void GameEventsFrameTick() {
  InstallGameEventsListenerImpl(/*force=*/false);
  RegisterPluginGameEvents();
}

// True only once the engine has actually delivered an event, so the log-line fallback keeps
// driving the round lifecycle until engine events are proven to flow.
bool GameEventsListenerInstalled() {
  return g_ok.load(std::memory_order_acquire) && g_eventsDelivered.load(std::memory_order_acquire);
}

void GameEventsObserveInterface(const char* name, void* iface) {
  if (!name || !*name || !iface) return;

  static std::atomic<bool> s_loggedSource2Server{false};
  static std::atomic<bool> s_loggedSource2ServerConfig{false};
  static std::atomic<bool> s_loggedGameEventSystem{false};

  // Cache stable interface pointers.
  if (std::strstr(name, "Source2ServerConfig") != nullptr) {
    g_source2ServerConfig.store(iface, std::memory_order_release);
    if (DebugEnabled()) {
      bool expected = false;
      if (s_loggedSource2ServerConfig.compare_exchange_strong(expected, true)) {
        Debug("game-events: observed %s => %p\n", name, iface);
      }
    }
    return;
  }
  if (std::strstr(name, "Source2Server") != nullptr) {
    g_source2Server.store(iface, std::memory_order_release);
    if (DebugEnabled()) {
      bool expected = false;
      if (s_loggedSource2Server.compare_exchange_strong(expected, true)) {
        Debug("game-events: observed %s => %p\n", name, iface);
      }
    }
    return;
  }
  if (std::strcmp(name, "GameEventSystemServerV001") == 0) {
    g_gameEventSystem.store(iface, std::memory_order_release);
    if (DebugEnabled()) {
      bool expected = false;
      if (s_loggedGameEventSystem.compare_exchange_strong(expected, true)) {
        Debug("game-events: observed %s => %p\n", name, iface);
      }
    }
    return;
  }
}

static std::mutex g_logTeamMu;
static std::unordered_map<int, int> g_logTeamBySlot;

void ObserveSlotTeamFromLog(int slot, int team) {
  if (slot < 0) return;
  std::lock_guard<std::mutex> lk(g_logTeamMu);
  g_logTeamBySlot[slot] = team;
}

static std::optional<int> LogTeamForSlot(int slot) {
  std::lock_guard<std::mutex> lk(g_logTeamMu);
  auto it = g_logTeamBySlot.find(slot);
  if (it == g_logTeamBySlot.end()) return std::nullopt;
  if (it->second == 2 || it->second == 3) return it->second;
  return std::nullopt;
}

std::optional<int> GetCsTeamNumForSlot(int slot) {
  if (slot < 0) return std::nullopt;
  if (!GameEventsListenerInstalled()) return LogTeamForSlot(slot);

  static std::optional<int> s_offTeamNum;
  static std::atomic<bool> s_lookedUp{false};
  bool expected = false;
  if (s_lookedUp.compare_exchange_strong(expected, true)) {
    // CS2: team number is on CCSPlayerController (2=T, 3=CT).
    s_offTeamNum = SchemaFindOffset("server", "CCSPlayerController", "m_iTeamNum");
  }
  if (!s_offTeamNum) return LogTeamForSlot(slot);

  std::lock_guard<EventsMutex> lk(g_mu);
  auto it = g_slotController.find(slot);
  if (it == g_slotController.end() || !it->second) return std::nullopt;
  if (auto v = ReadAt<int>(it->second, *s_offTeamNum)) {
    if (*v == 2 || *v == 3) return *v;
  }
  return std::nullopt;
}

void* GameEventsControllerForSlot(int slot) {
  if (slot < 0) return nullptr;
  std::lock_guard<EventsMutex> lk(g_mu);
  auto it = g_slotController.find(slot);
  return it == g_slotController.end() ? nullptr : it->second;
}

std::optional<int> GameEventsSlotForSteam(unsigned long long steamid64) {
  if (steamid64 == 0) return std::nullopt;
  std::lock_guard<EventsMutex> lk(g_mu);
  for (const auto& kv : g_slotController) {
    if (!kv.second) continue;
    const auto ident = GetSlotIdentity(kv.first);
    if (ident && ident->steamid64 == steamid64) return kv.first;
  }
  return std::nullopt;
}

sdk::IGameEventManager2* GameEventManagerVerified() {
  EnsureGameEventManagerResolved();
  return VerifiedMgr();
}

}  // namespace readyup

namespace readyup {
bool GameEventsInitHookInstalled() { return g_geHook != nullptr; }
}  // namespace readyup
