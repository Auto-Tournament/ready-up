#pragma once

namespace readyup {

// Suppress CS2 round termination (best-effort).
// When enabled, ReadyUp detours `CCSGameRules::TerminateRound` and returns early,
// preventing the game from ending the round (useful for warmup/practice freeplay).
//
// This is intentionally implemented by installing/uninstalling the detour rather
// than conditionally calling the original, so we don't need the exact engine ABI
// for the original function.
void SetRoundTerminationSuppressed(bool suppress);

// True if the detour is currently active.
bool RoundTerminationSuppressed();

// True once the TerminateRound detour is installed (it is installed on first suppression).
bool RoundTerminationHookInstalled();

}  // namespace readyup

