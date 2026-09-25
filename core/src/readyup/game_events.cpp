#include "readyup/game_events.h"

#include "readyup/plugin_api.h"
#include "readyup/plugin_loader.h"

#include "readyup/config.h"
#include "readyup/engine_surface.h"
#include "readyup/features.h"
#include "readyup/sdk/igameevents.h"
#include "readyup/logging.h"
#include "readyup/schema.h"
#include "readyup/signature_scan.h"
#include "readyup/slot_registry.h"
#include "readyup/real_server.h"


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


// Recursive: the listener re-enters this module on the same thread (player_spawn ->
// GetCsTeamNumForSlot). With a plain std::mutex those paths self-deadlock the game thread.
using EventsMutex = std::recursive_mutex;
EventsMutex g_mu;
std::unordered_map<int, void*> g_slotController;  // last controller seen per slot (this map)
std::string g_controllersMap;                     // map g_slotController belongs to
int g_roundNumber = 0;                            // RU_EVENT_ROUND_START/END round (this game)

// A new map drops the controllers and the round counter (game thread, under g_mu).
static void MaybeResetForMapLocked() {
  const std::string map = plugins::CurrentMap();
  if (map == g_controllersMap) return;
  g_controllersMap = map;
  g_slotController.clear();
  g_roundNumber = 0;
}

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

static void OnPlayerSpawnLocked(IGameEvent* ev) {
  if (!ev) return;
  const int slot = ev->GetPlayerSlot(CKV3MemberName("userid")).value;
  void* controller = ev->GetPlayerController(CKV3MemberName("userid"));
  if (slot >= 0 && controller) {
    g_slotController[slot] = controller;
    if (const uint64_t sid = SteamFromController(controller)) ObserveHumanSlot(sid, slot);
  }
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

    std::lock_guard<EventsMutex> lk(g_mu);
    MaybeResetForMapLocked();

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
      }
      return;
    }
    if (std::strcmp(name, "begin_new_match") == 0) {
      g_roundNumber = 0;  // warmup end / mp_restartgame
      return;
    }
    if (std::strcmp(name, "round_start") == 0) {
      g_roundNumber += 1;
      PostPluginEvent(RU_EVENT_ROUND_START, event, g_roundNumber);
      return;
    }
    if (std::strcmp(name, "round_end") == 0) {
      PostPluginEvent(RU_EVENT_ROUND_END, event, g_roundNumber);
      return;
    }
    if (std::strcmp(name, "player_spawn") == 0) {
      OnPlayerSpawnLocked(event);
      return;
    }
  }
};

ListenerImpl g_listener;


}  // namespace

// Events ListenerImpl consumes. `core` events must all be registered before we report
// "installed"; the others are best-effort.
struct ListenedEvent {
  const char* name;
  bool core;
};
// Plugins get more through subscribe_game_event (RegisterPluginGameEvents).
static constexpr ListenedEvent kListenedEvents[] = {
    {"round_start", true},
    {"round_end", true},
    {"player_spawn", true},
    {"player_team", false},
    {"player_connect_full", false},
    {"player_disconnect", false},
    {"begin_new_match", false},
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
