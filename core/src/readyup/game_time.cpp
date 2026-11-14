#include "readyup/game_time.h"

#include "readyup/engine_surface.h"
#include "readyup/logging.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace readyup::game_time {
namespace {

constexpr const char* kSlotName = "ISource2Server::SetGlobals";
constexpr size_t kCurtimeOffset = 48;  // CGlobalVars::curtime, as HLAE reads it on the client
constexpr int kTrustAfter = 64;        // a second of ticks that each moved curtime forward

void* const* g_globalsSlot = nullptr;  // where SetGlobals stored its CGlobalVars*
float g_last = NAN;
int g_good = 0;
bool g_trusted = false;

float Read() {
  const unsigned char* globals = g_globalsSlot ? static_cast<const unsigned char*>(*g_globalsSlot) : nullptr;
  if (!globals) return NAN;
  float v;
  std::memcpy(&v, globals + kCurtimeOffset, sizeof v);
  return v;
}

}  // namespace

void Init(void* source2Server) {
  if (g_globalsSlot || !source2Server) return;
  int index = -1;
  std::string why;
  if (!VerifyEngineVtableSlot(kSlotName, source2Server, &index, &why)) {
    Print("game-time: no server game time (%s)\n", why.c_str());
    return;
  }
  // The verified anchor: `mov [rip+disp32], rsi` within the first 64 bytes.
  const unsigned char* fn = static_cast<const unsigned char*>((*static_cast<void***>(source2Server))[index]);
  for (int i = 0; i + 7 <= 64; ++i) {
    if (fn[i] != 0x48 || fn[i + 1] != 0x89 || fn[i + 2] != 0x35) continue;
    int32_t disp;
    std::memcpy(&disp, fn + i + 3, 4);
    g_globalsSlot = reinterpret_cast<void* const*>(fn + i + 7 + disp);
    Debug("game-time: CGlobalVars* at %p (SetGlobals vtbl[%d])\n", static_cast<const void*>(g_globalsSlot), index);
    return;
  }
}

void Frame() {
  const float now = Read();
  if (!std::isfinite(now) || now < 0) {
    g_good = 0, g_trusted = false, g_last = NAN;
    return;
  }
  // A map load restarts curtime; anything else that isn't a small step forward starts the count over.
  const float step = now - g_last;
  if (std::isfinite(step) && step > 0 && step < 0.1f) {
    if (!g_trusted && ++g_good >= kTrustAfter) {
      g_trusted = true;
      Print("game-time: server game time verified (curtime %.2f)\n", now);
    }
  } else if (!g_trusted || now < g_last) {
    g_good = 0;
  }
  g_last = now;
}

double Now() { return g_trusted ? static_cast<double>(g_last) : NAN; }

}  // namespace readyup::game_time
