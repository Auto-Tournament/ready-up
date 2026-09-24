#include "readyup/client_print.h"

#include "readyup/config.h"
#include "readyup/engine_surface.h"
#include "readyup/features.h"
#include "readyup/game_events.h"
#include "readyup/logging.h"

#include <atomic>

namespace readyup {
namespace {

// UTIL_ClientPrintAll(int dest, const char* msg, p1, p2, p3, p4) — builds a reliable
// all-players recipient filter and forwards to UTIL_ClientPrintFilter. The trailing
// pointer is harmless padding kept from CounterStrikeSharp's managed signature.
using UtilClientPrintAllFn = void (*)(int dest, const char* msg, void*, void*, void*, void*, void*);

// ClientPrint(CBasePlayerController* controller, int dest, const char* msg, p1, p2, p3, p4)
// 1.41.8.3 @ rva 0x1802df0: null-checks rdi, builds a single-recipient reliable filter
// from the controller's slot, then calls UTIL_ClientPrintFilter. (The CDN "ClientPrint"
// signature hits an unrelated constructor at rva 0x1d11eb0; do not use it.)
using ClientPrintFn = void (*)(void* controller, int dest, const char* msg, const char*, const char*, const char*,
                               const char*);

constexpr int kHudPrintTalk = 3;

std::atomic<UtilClientPrintAllFn> g_fnAll{nullptr};
std::atomic<ClientPrintFn> g_fnOne{nullptr};

template <typename Fn>
Fn Resolve(std::atomic<Fn>& slot, const char* name) {
  Fn fn = slot.load(std::memory_order_acquire);
  if (fn) return fn;
  // EngineFunction caches both success and failure once the real server is loaded.
  fn = reinterpret_cast<Fn>(EngineFunction(name));
  if (fn) slot.store(fn, std::memory_order_release);
  return fn;
}

}  // namespace

bool ClientPrintChat(int slot, const char* msg) {
  if (!msg || !*msg || slot < 0) return false;
  if (!FeatureEnabled(Feature::PlayerChatPrint)) return false;
  ClientPrintFn fn = Resolve(g_fnOne, "ClientPrint");
  if (!fn) return false;
  void* controller = GameEventsControllerForSlot(slot);
  if (!controller) {
    if (DebugEnabled()) Debug("clientprint: no known controller for slot %d\n", slot);
    return false;
  }
  fn(controller, kHudPrintTalk, msg, nullptr, nullptr, nullptr, nullptr);
  return true;
}

bool ClientPrintAvailable() {
  return Resolve(g_fnAll, "UTIL_ClientPrintAll") != nullptr;
}

bool ClientPrintAllChat(const char* msg) {
  if (!msg || !*msg) return false;
  UtilClientPrintAllFn fn = Resolve(g_fnAll, "UTIL_ClientPrintAll");
  if (!fn) return false;
  fn(kHudPrintTalk, msg, nullptr, nullptr, nullptr, nullptr, nullptr);
  return true;
}

}  // namespace readyup
