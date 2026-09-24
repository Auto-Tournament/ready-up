#include "readyup/console_command.h"

#include "readyup/config.h"
#include "readyup/logging.h"

#include <dlfcn.h>

namespace readyup {
namespace {

using CreateInterfaceFn = void* (*)(const char* name, int* returnCode);

}  // namespace

void TryRegisterConsoleCommands() {
  // Source2 console command registration is not stable across builds and typically relies on
  // SDK headers (ConCommand/ICvar). Ready Up keeps this best-effort and will not crash if the
  // expected APIs are unavailable.

  // We can, however, probe for a cvar factory and log what we find.
  using GetICVarFactoryFn = CreateInterfaceFn (*)();
  auto getFactory = reinterpret_cast<GetICVarFactoryFn>(dlsym(RTLD_DEFAULT, "Import_GetICVarFactory"));
  if (!getFactory) {
    if (DebugEnabled()) {
      PrintLine("console: Import_GetICVarFactory not found; native `ru` console command disabled (use `.ru ...` in chat).");
    }
    return;
  }

  CreateInterfaceFn factory = getFactory();
  if (!factory) {
    if (DebugEnabled()) {
      PrintLine("console: ICvar factory is null; native `ru` console command disabled.");
    }
    return;
  }

  void* icvar = factory("VEngineCvar007", nullptr);
  if (!icvar) {
    // Try a couple of plausible version bumps.
    icvar = factory("VEngineCvar008", nullptr);
  }
  if (!icvar) {
    icvar = factory("VEngineCvar006", nullptr);
  }

  if (!icvar) {
    if (DebugEnabled()) {
      PrintLine("console: VEngineCvar interface not available; native `ru` console command disabled.");
    }
    return;
  }

  // At this point we have an ICvar*, but we don't have a safe, ABI-stable way to construct and
  // register a ConCommand without matching SDK headers for this exact build.
  if (DebugEnabled()) {
    PrintLine("console: ICvar found, but ConCommand registration is not implemented (ABI risk). Using `.ru ...` chat commands only.");
  }
}

}  // namespace readyup

