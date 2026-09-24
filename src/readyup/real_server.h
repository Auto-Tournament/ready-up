#pragma once

namespace readyup {

// Loads Valve's real `libserver.so` and exposes its CreateInterface + other exports.
void EnsureRealServerLoaded();
void* RealServerHandle();
void* RealCreateInterface(const char* name, int* returnCode);

}  // namespace readyup

