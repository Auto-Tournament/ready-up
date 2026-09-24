#include "readyup/round_termination_hook.h"

#include "readyup/config.h"
#include "readyup/engine_surface.h"
#include "readyup/features.h"
#include "readyup/logging.h"
#include "readyup/signature_scan.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

#include "third_party/funchook/include/funchook.h"

namespace readyup {
namespace {

// CCSGameRules::TerminateRound(this, float delay, RoundEndReason reason, <ptr>, <uint>).
// In 1.41.8.3 the function spills rdx as a 64-bit value, so the 3rd integer argument is
// pointer-sized. We forward every integer argument register (rdi..r9) and xmm0 untouched
// so the original sees exactly what its caller passed.
using TerminateRoundFn = void (*)(void* thisptr, float delay, int reason, void* a3, uint64_t a4, uint64_t a5,
                                  uint64_t a6);

TerminateRoundFn g_term_orig = nullptr;
TerminateRoundFn g_term_trampoline = nullptr;  // rewritten to trampoline by funchook_prepare
funchook_t* g_hook = nullptr;

std::atomic<bool> g_active{false};
std::atomic<bool> g_resolved{false};
std::atomic<bool> g_suppressCalls{false};

static long long NowMs() {
  using Clock = std::chrono::system_clock;
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

static void DetourTerminateRound(void* thisptr, float delay, int reason, void* a3, uint64_t a4, uint64_t a5,
                                 uint64_t a6) {
  // Engine-level "round ends / termination" entry point.
  // We either suppress it (warmup/practice) or forward to the original (live/knife/etc).
  static std::atomic<uint64_t> s_calls{0};
  static std::atomic<uint64_t> s_suppressed{0};
  static std::atomic<uint64_t> s_forwarded{0};
  static std::atomic<long long> s_lastLogMs{0};

  const bool suppress = g_suppressCalls.load(std::memory_order_acquire);
  s_calls.fetch_add(1, std::memory_order_relaxed);
  if (suppress) s_suppressed.fetch_add(1, std::memory_order_relaxed);
  else s_forwarded.fetch_add(1, std::memory_order_relaxed);

  if (DebugEnabled()) {
    const long long now = NowMs();
    const long long last = s_lastLogMs.load(std::memory_order_relaxed);
    // Rate-limit: once per ~2s (TerminateRound can be called frequently).
    if (last == 0 || (now - last) >= 2000) {
      s_lastLogMs.store(now, std::memory_order_relaxed);
      Debug("roundterm: TerminateRound called (suppress=%d delay=%.3f reason=%d) calls=%llu suppressed=%llu forwarded=%llu\n",
            suppress ? 1 : 0,
            delay,
            reason,
            static_cast<unsigned long long>(s_calls.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(s_suppressed.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(s_forwarded.load(std::memory_order_relaxed)));
    }
  }

  if (!suppress) {
    // Forward to original if suppression is disabled.
    if (g_term_trampoline) g_term_trampoline(thisptr, delay, reason, a3, a4, a5, a6);
  }
}

static bool ResolveTarget() {
  if (g_resolved.load(std::memory_order_acquire)) return true;

  // Unresolved => the round_term_suppression feature is off and says so once.
  if (!FeatureEnabled(Feature::RoundTermSuppression)) return false;
  void* p = EngineFunction("CCSGameRules_TerminateRound");
  if (!p) return false;

  g_term_orig = reinterpret_cast<TerminateRoundFn>(p);
  g_term_trampoline = g_term_orig;
  g_resolved.store(true, std::memory_order_release);
  return true;
}

static bool InstallHook() {
  if (g_active.load(std::memory_order_acquire)) return true;
  if (!ResolveTarget()) return false;

  if (DebugEnabled()) DebugLine("roundterm: attempting hook install...");

  if (!g_hook) {
    g_hook = funchook_create();
    if (!g_hook) {
      PrintLine("roundterm: funchook_create failed");
      return false;
    }
  }

  // Prepare may be called multiple times if we destroy/recreate hook, but we keep it simple:
  // - if already prepared/installed, g_active would be true and we'd have returned above.
  int rv = funchook_prepare(g_hook, (void**)&g_term_trampoline, (void*)&DetourTerminateRound);
  if (rv != 0) {
    Print("roundterm: funchook_prepare failed (%d): %s\n", rv, funchook_error_message(g_hook));
    funchook_destroy(g_hook);
    g_hook = nullptr;
    // Reset trampoline pointer for a clean retry.
    g_term_trampoline = g_term_orig;
    return false;
  }

  rv = funchook_install(g_hook, 0);
  if (rv != 0) {
    Print("roundterm: funchook_install failed (%d): %s\n", rv, funchook_error_message(g_hook));
    funchook_destroy(g_hook);
    g_hook = nullptr;
    g_term_trampoline = g_term_orig;
    return false;
  }

  g_active.store(true, std::memory_order_release);
  if (DebugEnabled()) {
    Debug("roundterm: hook installed (orig=%p trampoline=%p)\n",
          reinterpret_cast<void*>(g_term_orig),
          reinterpret_cast<void*>(g_term_trampoline));
  } else {
    PrintLine("roundterm: hook installed");
  }
  return true;
}

[[maybe_unused]] static void UninstallHook() {
  if (!g_active.load(std::memory_order_acquire)) return;
  if (!g_hook) {
    g_active.store(false, std::memory_order_release);
    return;
  }

  const int rv = funchook_uninstall(g_hook, 0);
  if (rv != 0 && DebugEnabled()) {
    Print("roundterm: funchook_uninstall failed (%d): %s\n", rv, funchook_error_message(g_hook));
  }
  funchook_destroy(g_hook);
  g_hook = nullptr;

  // Restore function pointer for next install attempt.
  g_term_trampoline = g_term_orig;
  g_active.store(false, std::memory_order_release);

  if (DebugEnabled()) PrintLine("roundterm: hook uninstalled");
  else PrintLine("roundterm: hook uninstalled");
}

}  // namespace

void SetRoundTerminationSuppressed(bool suppress) {
  // Keep the detour installed once it succeeds; toggle suppression via atomic flag.
  // This gives better observability (we can log terminate attempts even when not suppressing)
  // and avoids repeated install/uninstall churn.
  static std::atomic<int> s_last{-1};
  const int cur = suppress ? 1 : 0;
  const int prev = s_last.exchange(cur);
  if (DebugEnabled() && prev != cur) {
    Debug("roundterm: suppress flag -> %d\n", cur);
  }

  if (suppress) {
    if (InstallHook()) {
      g_suppressCalls.store(true, std::memory_order_release);
    }
    return;
  }

  // Not suppressing: forward TerminateRound to original.
  g_suppressCalls.store(false, std::memory_order_release);
}

bool RoundTerminationSuppressed() {
  return g_active.load(std::memory_order_acquire) && g_suppressCalls.load(std::memory_order_acquire);
}

}  // namespace readyup


namespace readyup {
bool RoundTerminationHookInstalled() { return g_active.load(std::memory_order_acquire); }
}  // namespace readyup
