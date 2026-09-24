#include "readyup/host_say_hook.h"

#include "readyup/ccommand.h"
#include "readyup/config.h"
#include "readyup/engine_surface.h"
#include "readyup/game_events.h"
#include "readyup/logging.h"
#include "readyup/server_game_clients_hook.h"
#include "readyup/slot_registry.h"
#include "readyup/ru_router.h"

#include <atomic>
#include <cstdint>
#include <string>

#include "third_party/funchook/include/funchook.h"

namespace readyup {
namespace {

using HostSayFn = void (*)(void* controller, void* cmdRef, bool teamonly, int unk1, const char* unk2);
HostSayFn g_hostSay = nullptr;  // will be rewritten to trampoline after prepare
HostSayFn g_hostSay_orig = nullptr;

std::atomic<bool> g_installed{false};
std::atomic<bool> g_hook_ok{false};
funchook_t* g_hook = nullptr;

// Sender slot = the slot whose controller (as seen in engine events) is this pointer.
// No vtable guessing: the old code searched the controller's vtable for a dladdr() symbol name
// containing "GetEntityIndex", which never resolves on stripped Valve builds.
int SlotForController(void* controller) {
  if (!controller) return -1;
  for (int slot = 0; slot < 64; ++slot) {
    if (GameEventsControllerForSlot(slot) == controller) return slot;
  }
  return -1;
}

static void DetourHostSay(void* controller, void* cmdRef, bool teamonly, int unk1, const char* unk2) {
  // Always let original run first (match CSS default behavior).
  if (g_hostSay) g_hostSay(controller, cmdRef, teamonly, unk1, unk2);

  // If the ClientCommand hook is installed it routes `.ru` with a reliable sender; the log
  // listener covers everything else. Host_Say only helps when neither can.
  if (ServerGameClientsHookInstalled()) return;

  // Arguments via the verified CCommand layout only (no pointer-scanning for strings).
  const auto argv = ReadCCommandArgs(cmdRef);
  if (!argv || argv->size() < 2) return;
  const std::string text = CCommandArgString(*argv);
  if (text.rfind(".ru", 0) != 0) return;
  Debug("hostsay: saw .ru teamonly=%d unk1=%d unk2=%p\n", teamonly ? 1 : 0, unk1, (const void*)unk2);

  const int slot = SlotForController(controller);
  const auto id = slot >= 0 ? GetSlotIdentity(slot) : std::nullopt;
  // Without a sender identity, leave it to the in-process log listener (it has SteamID/name).
  if (!id || id->steamid64 == 0) {
    DebugLine("hostsay: skipping routing (no sender identity)");
    return;
  }
  RouteChatCommand(id->steamid64, id->name, text);
}

}  // namespace

void InstallHostSayHook() {
  if (g_installed.exchange(true)) return;

  // Unresolved => nothing to hook; chat still works through the log listener / ClientCommand.
  void* p = EngineFunction("Host_Say");
  if (!p) return;

  g_hostSay_orig = reinterpret_cast<HostSayFn>(p);
  g_hostSay = g_hostSay_orig;

  g_hook = funchook_create();
  if (!g_hook) {
    PrintLine("hostsay: funchook_create failed");
    return;
  }

  int rv = funchook_prepare(g_hook, (void**)&g_hostSay, (void*)&DetourHostSay);
  if (rv != 0) {
    Print("hostsay: funchook_prepare failed (%d): %s\n", rv, funchook_error_message(g_hook));
    funchook_destroy(g_hook);
    g_hook = nullptr;
    return;
  }

  rv = funchook_install(g_hook, 0);
  if (rv != 0) {
    Print("hostsay: funchook_install failed (%d): %s\n", rv, funchook_error_message(g_hook));
    funchook_destroy(g_hook);
    g_hook = nullptr;
    return;
  }

  g_hook_ok.store(true, std::memory_order_release);
  Print("hostsay: hooked Host_Say at %p (trampoline=%p)\n", reinterpret_cast<void*>(g_hostSay_orig), reinterpret_cast<void*>(g_hostSay));
}

bool HostSayHookInstalled() {
  return g_hook_ok.load(std::memory_order_acquire);
}

}  // namespace readyup
