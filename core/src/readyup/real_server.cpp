#include "readyup/real_server.h"

#include "readyup/config.h"
#include "readyup/logging.h"
#include "readyup/path.h"

#include <dlfcn.h>
#include <unistd.h>

#include <mutex>
#include <string>

namespace readyup {
namespace {

using CreateInterfaceFn = void* (*)(const char* name, int* returnCode);

std::once_flag g_loadOnce;
void* g_realHandle = nullptr;
CreateInterfaceFn g_realCreateInterface = nullptr;
std::string g_realLoadedPath;

void LoadRealServerModule() {
  const char* overridePath = std::getenv("READYUP_REAL_SERVER_PATH");

  std::string libPath;
  if (overridePath && *overridePath) {
    libPath = overridePath;
    if (DebugEnabled()) {
      Print("real server override path: %s\n", libPath.c_str());
    }
  } else {
    const std::string dir = GetThisModuleDir();
    if (dir.empty()) {
      PrintLine("Could not resolve module directory (dladdr failed).");
      return;
    }

    // linuxsteamrt64 -> bin -> readyup -> csgo
    const std::string valveFallback = dir + "/../../../bin/linuxsteamrt64/libserver.so";
    libPath = valveFallback;

    if (DebugEnabled()) {
      Print("module dir: %s\n", dir.c_str());
      Print("candidate valve: %s (readable=%s)\n",
            valveFallback.c_str(),
            (access(valveFallback.c_str(), R_OK) == 0) ? "yes" : "no");
      Print("selected real: %s\n", libPath.c_str());
    }
  }

  if (!g_realHandle) {
    if (DebugEnabled()) {
      Print("dlopen(real) %s\n", libPath.c_str());
    }

    g_realHandle = dlopen(libPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!g_realHandle) {
      const char* err = dlerror();
      std::string msg = "dlopen(real libserver) failed: ";
      msg += (err ? err : "(no dlerror)");
      msg += " (path=";
      msg += libPath;
      msg += ")";
      PrintLine(msg.c_str());
      return;
    }

    if (DebugEnabled()) {
      Print("dlopen(real) ok: handle=%p\n", g_realHandle);
    }
  }

  g_realLoadedPath = libPath;

  void* sym = dlsym(g_realHandle, "CreateInterface");
  if (!sym) {
    const char* err = dlerror();
    std::string msg = "dlsym(CreateInterface) failed: ";
    msg += (err ? err : "(no dlerror)");
    PrintLine(msg.c_str());
    return;
  }

  g_realCreateInterface = reinterpret_cast<CreateInterfaceFn>(sym);
  if (DebugEnabled()) {
    Print("dlsym(CreateInterface)=%p\n", sym);
    Print("CreateInterface symbol owner: %s\n", DladdrDescribe(sym).c_str());
  }

  PrintLine("Loaded real libserver.so and resolved CreateInterface.");
}

}  // namespace

void EnsureRealServerLoaded() {
  std::call_once(g_loadOnce, LoadRealServerModule);
}

void* RealServerHandle() {
  EnsureRealServerLoaded();
  return g_realHandle;
}

void* RealCreateInterface(const char* name, int* returnCode) {
  EnsureRealServerLoaded();
  if (!g_realCreateInterface) {
    PrintLine("CreateInterface called but real CreateInterface is unavailable.");
    if (returnCode) *returnCode = 1;
    return nullptr;
  }
  return g_realCreateInterface(name, returnCode);
}

}  // namespace readyup

