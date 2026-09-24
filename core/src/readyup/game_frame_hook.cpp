#include "readyup/game_frame_hook.h"

#include "readyup/entity.h"

#include "readyup/config.h"
#include "readyup/engine_surface.h"
#include "readyup/game_timers.h"
#include "readyup/features.h"
#include "readyup/game_events.h"
#include "readyup/logging.h"
#include "readyup/modes.h"
#include "readyup/plugin_loader.h"
#include "readyup/real_server.h"
#include "readyup/scrim_flow.h"
#include "readyup/ready_hud.h"
#include "readyup/selftest.h"
#include "readyup/welcome.h"

#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <mutex>

namespace readyup {
namespace {

constexpr const char* kSlotName = "ISource2Server::GameFrame";

// Matches CS2 HL2SDK: ISource2Server::GameFrame(bool simulating, bool bFirstTick, bool bLastTick)
using GameFrameFn = void (*)(void* thisptr, bool simulating, bool bFirstTick, bool bLastTick);
GameFrameFn g_orig = nullptr;

std::mutex g_installMu;
std::atomic<bool> g_ok{false};
std::atomic<bool> g_failed{false};
std::string g_failDetail;  // guarded by g_installMu
std::atomic<unsigned long long> g_simTicks{0};

static bool PatchVtable(void** vtable, int index, void* replacement, void** outOld) {
  if (!vtable || index < 0) return false;
  void** slot = &vtable[index];
  const uintptr_t addr = reinterpret_cast<uintptr_t>(slot);
  const uintptr_t page = addr & ~(static_cast<uintptr_t>(getpagesize() - 1));
  if (mprotect(reinterpret_cast<void*>(page), static_cast<size_t>(getpagesize()), PROT_READ | PROT_WRITE) != 0) {
    return false;
  }
  if (outOld) *outOld = *slot;
  *slot = replacement;
  mprotect(reinterpret_cast<void*>(page), static_cast<size_t>(getpagesize()), PROT_READ);
  return true;
}

static void Hook_GameFrame(void* thisptr, bool simulating, bool bFirstTick, bool bLastTick) {
  if (g_orig) g_orig(thisptr, simulating, bFirstTick, bLastTick);

  // Our logic is best-effort. Every feature below gates itself on its engine-surface
  // dependencies (features.cpp; one log line when one is disabled).
  const bool plugins = FeatureEnabled(Feature::Plugins);
  // Map-end / series-end timers and the demo stop (game_timers.h) also run while the
  // server is not simulating (e.g. empty after the series-end kick).
  readyup::GameTimersFrameTick();
  if (!simulating) {
    // Plugin load/unload + queued commands/events still run while not simulating.
    if (plugins) readyup::plugins::Frame(/*simulating=*/false);
    return;
  }
  if (g_simTicks.fetch_add(1, std::memory_order_relaxed) == 0 && DebugEnabled()) {
    DebugLine("gameframe: first simulating tick observed");
  }
  if (FeatureEnabled(Feature::MatchFlow)) {
    // Tick() also registers engine event listeners (GameEventsFrameTick, gated by "events").
    readyup::Tick();
    // Scrim flow + `state:` log; outside Tick() (which holds the modes mutex).
    readyup::ScrimTick();
  } else {
    readyup::GameEventsFrameTick();
  }
  // Runs after Tick() returns (Tick holds the modes mutex; WelcomeTick reads mode).
  if (FeatureEnabled(Feature::WelcomeHtml)) readyup::WelcomeTick();
  // Ready list / knife pick panel (skips players whose welcome screen is up).
  if (FeatureEnabled(Feature::ReadyHud)) readyup::ReadyHudTick();
  // Entity primitives status (once the entity system verified on the first map). Skins itself
  // is a plugin now (plugins/skins); its per-tick work runs in plugins::Frame below.
  if (readyup::entity::EntitySystemReady()) readyup::entity::LogEngineStatusOnce();
  // Plugins last: pending load/unload/reload (a safe point: no plugin code is on the
  // stack), then queued commands/events, then per-tick callbacks.
  if (plugins) readyup::plugins::Frame(/*simulating=*/true);
  // READYUP_SELFTEST_AND_QUIT: runs the selftest once the first map is up, then quits.
  readyup::SelftestFrameTick();
}

}  // namespace

void InstallGameFrameHook(void* source2ServerIface) {
  if (!source2ServerIface || g_ok.load(std::memory_order_acquire)) return;
  std::lock_guard<std::mutex> lk(g_installMu);
  if (g_ok.load(std::memory_order_acquire)) return;

  int index = -1;
  std::string why;
  if (!VerifyEngineVtableSlot(kSlotName, source2ServerIface, &index, &why)) {
    // Unverified slot: final, logged once. Another interface whose factory name merely contains
    // "Source2Server" (e.g. Source2ServerConfig001) is skipped quietly.
    const VtableVerdict v = EngineVtableVerdict(kSlotName);
    if (!v.ok && !g_failed.exchange(true)) {
      g_failDetail = why;
      Print("gameframe: GameFrame hook NOT installed (%s)\n", why.c_str());
    } else if (v.ok) {
      Debug("gameframe: skipping interface %p: %s\n", source2ServerIface, why.c_str());
    }
    return;
  }

  void** vt = *reinterpret_cast<void***>(source2ServerIface);
  void* old = nullptr;
  if (!PatchVtable(vt, index, reinterpret_cast<void*>(&Hook_GameFrame), &old) || !old) {
    g_failDetail = "mprotect/patch of the vtable slot failed";
    g_failed.store(true);
    PrintLine("gameframe: failed to patch vtable for GameFrame.");
    return;
  }

  g_orig = reinterpret_cast<GameFrameFn>(old);
  g_ok.store(true, std::memory_order_release);
  MarkEngineVtablePatched(kSlotName, old);
  Print("gameframe: hooked GameFrame vtbl[%d] (verified) old=%p new=%p\n", index, old,
        reinterpret_cast<void*>(&Hook_GameFrame));
}

void InstallGameFrameHookAuto() {
  int rc = 0;
  void* iface = RealCreateInterface("Source2Server001", &rc);
  if (!iface) {
    if (DebugEnabled()) Print("gameframe: RealCreateInterface(Source2Server001) failed (rc=%d)\n", rc);
    return;
  }
  InstallGameFrameHook(iface);
}

bool GameFrameHookInstalled() {
  return g_ok.load(std::memory_order_acquire);
}

bool GameFrameHookFailed(std::string* detail) {
  if (!g_failed.load(std::memory_order_acquire)) return false;
  if (detail) {
    std::lock_guard<std::mutex> lk(g_installMu);
    *detail = g_failDetail;
  }
  return true;
}

unsigned long long GameFrameSimulatingTicks() {
  return g_simTicks.load(std::memory_order_relaxed);
}

}  // namespace readyup
