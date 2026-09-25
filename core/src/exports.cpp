#include "readyup/banner.h"
#include "readyup/client_command_hook.h"
#include "readyup/command_buffer_hook.h"
#include "readyup/config.h"
#include "readyup/console_command.h"
#include "readyup/crash_handler.h"
#include "readyup/disabled.h"
#include "readyup/engine_surface.h"
#include "readyup/selftest.h"
#include "readyup/game_events.h"
#include "readyup/game_frame_hook.h"
#include "readyup/log_receiver.h"
#include "readyup/logging.h"
#include "readyup/path.h"
#include "readyup/real_server.h"
#include "readyup/server_game_clients_hook.h"
#include "readyup/host_say_hook.h"
#include "readyup/sigtest.h"
#include "readyup/status_feed.h"

#include <dlfcn.h>

#include <cstring>
#include <mutex>

namespace {
std::once_flag g_loadOnce;
}  // namespace

extern "C" {

__attribute__((visibility("default"))) const char* BinaryProperties_GetValue(const char* key) {
  if (readyup::DebugEnabled()) {
    readyup::Print("export BinaryProperties_GetValue(%s)\n", key ? key : "(null)");
  }
  std::call_once(g_loadOnce, readyup::EnsureRealServerLoaded);
  void* h = readyup::RealServerHandle();
  if (!h) return nullptr;
  using Fn = const char* (*)(const char*);
  auto fn = reinterpret_cast<Fn>(dlsym(h, "BinaryProperties_GetValue"));
  return fn ? fn(key) : nullptr;
}

__attribute__((visibility("default"))) int GetResourceManifestCount() {
  if (readyup::DebugEnabled()) {
    readyup::Print("export GetResourceManifestCount()\n");
  }
  std::call_once(g_loadOnce, readyup::EnsureRealServerLoaded);
  void* h = readyup::RealServerHandle();
  if (!h) return 0;
  using Fn = int (*)();
  auto fn = reinterpret_cast<Fn>(dlsym(h, "GetResourceManifestCount"));
  return fn ? fn() : 0;
}

__attribute__((visibility("default"))) const char** GetResourceManifests() {
  if (readyup::DebugEnabled()) {
    readyup::Print("export GetResourceManifests()\n");
  }
  std::call_once(g_loadOnce, readyup::EnsureRealServerLoaded);
  void* h = readyup::RealServerHandle();
  if (!h) return nullptr;
  using Fn = const char** (*)();
  auto fn = reinterpret_cast<Fn>(dlsym(h, "GetResourceManifests"));
  return fn ? fn() : nullptr;
}

__attribute__((visibility("default"))) void InstallSchemaBindings() {
  if (readyup::DebugEnabled()) {
    readyup::Print("export InstallSchemaBindings()\n");
  }
  std::call_once(g_loadOnce, readyup::EnsureRealServerLoaded);
  void* h = readyup::RealServerHandle();
  if (!h) return;
  using Fn = void (*)();
  auto fn = reinterpret_cast<Fn>(dlsym(h, "InstallSchemaBindings"));
  if (fn) fn();
}

__attribute__((visibility("default"))) void ExtractModuleMetadata() {
  if (readyup::DebugEnabled()) {
    readyup::Print("export ExtractModuleMetadata()\n");
  }
  std::call_once(g_loadOnce, readyup::EnsureRealServerLoaded);
  void* h = readyup::RealServerHandle();
  if (!h) return;
  using Fn = void (*)();
  auto fn = reinterpret_cast<Fn>(dlsym(h, "ExtractModuleMetadata"));
  if (fn) fn();
}

// Engine entrypoint.
extern "C" __attribute__((visibility("default"))) void* CreateInterface(const char* name, int* returnCode) {
  static thread_local int s_depth = 0;
  struct DepthGuard {
    int& d;
    explicit DepthGuard(int& depth) : d(depth) { ++d; }
    ~DepthGuard() { --d; }
  } guard(s_depth);

  if (s_depth > 16) {
    readyup::PrintLine("CreateInterface recursion detected (depth>16); bailing to avoid stack overflow.");
    if (returnCode) *returnCode = 1;
    return nullptr;
  }

  if (readyup::DebugEnabled()) {
    readyup::Print("CreateInterface request: %s\n", name ? name : "(null)");
  }

  readyup::InstallCrashHandlersMaybe();
  std::call_once(g_loadOnce, readyup::EnsureRealServerLoaded);

  void* out = readyup::RealCreateInterface(name, returnCode);
  if (readyup::DebugEnabled()) {
    readyup::Print("CreateInterface result: %p (rc=%d)\n", out, returnCode ? *returnCode : -999);
  }

  // Hook player chat at the real source: IServerGameClients::ClientCommand.
  // The interface name varies by build; CS2 commonly requests Source2GameClients001.
  if (!readyup::IsDisabled() && out && name) {
    // Feed interface pointers to subsystems that can make use of them.
    readyup::GameEventsObserveInterface(name, out);

    if (std::strstr(name, "Source2GameClients") != nullptr || std::strstr(name, "ServerGameClients") != nullptr) {
      readyup::InstallServerGameClientsHook(out);
    }
    if (std::strstr(name, "Source2Server") != nullptr) {
      readyup::InstallGameFrameHook(out);
      // When server interfaces become available, retry installing game events.
      readyup::InstallGameEventsListener();
    }
  }
  return out;
}

}  // extern "C"

__attribute__((constructor)) static void readyup_ctor() {
  readyup::LogBanner();
  const std::string p = readyup::GetThisModulePath();
  if (!p.empty()) {
    readyup::Print("loaded from: %s\n", p.c_str());
  }
  readyup::PrintLine("libserver.so loaded.");

  readyup::InstallCrashHandlersMaybe();
  readyup::EnsureRealServerLoaded();
  // READYUP_SELFTEST_AND_QUIT: arm the watchdog before anything can fail.
  readyup::StartSelftestWatchdogIfRequested();

  // Fail-closed without taking the server down:
  // - If core signatures don't match, disable Ready Up side effects (become inert).
  // - The server should continue to run normally with the real libserver.so.
  if (!readyup::RunSigTest(/*verbose=*/false)) {
    readyup::Disable("sigtest failed (signature mismatch / missing)");
    readyup::PrintLine("Ready Up disabled: sigtest failed. Server will run without Ready Up hooks.");
    // The status endpoint still starts (own thread, no engine access) so /health reports it.
    readyup::status_feed::StartAtLoad();
    return;
  }

  // Verify every patched/called virtual slot up front (RTTI vtable + slot anchors, same check as
  // readyup_sigcheck). Hooks below only patch slots that verified; features needing a slot that
  // did not verify disable themselves with one log line.
  readyup::VerifyAllEngineVtables();

  // Local status endpoint (docs/FLEET.md §17): its own thread, fed from GameFrame.
  readyup::status_feed::StartAtLoad();

  // (Persisted MAT settings and boot-time match recovery belong to plugins/match, which loads
  // on the first server frame.)
  readyup::InstallClientCommandHook();
  readyup::TryRegisterConsoleCommands();
  readyup::InstallCommandBufferHook();

  // Proactively grab and hook ISource2GameClients (CounterStrikeSharp uses "Source2GameClients001").
  // Some builds won't request this interface through our CreateInterface wrapper early enough,
  // which breaks `.ru` routing via ClientCommand slot identity. The slot is verified first.
  {
    int rc = 0;
    void* iface = readyup::RealCreateInterface("Source2GameClients001", &rc);
    if (iface) {
      readyup::InstallServerGameClientsHook(iface);
    } else if (readyup::DebugEnabled()) {
      readyup::Print("admin-prefix: RealCreateInterface(Source2GameClients001) failed (rc=%d)\n", rc);
    }
  }

  // Proactively hook server GameFrame so we can tick on server thread.
  readyup::InstallGameFrameHookAuto();

  readyup::InstallHostSayHook();

  // Best-effort: observe logs for slot identities + scores/map changes.
  readyup::StartLogReceiver();

  // Engine event listener (round lifecycle + stats).
  readyup::InstallGameEventsListener();
}

