#pragma once

namespace readyup {

constexpr const char* kLogPrefix = "[ReadyUp]";

// Print without prefix (useful for banners / raw output).
void PrintRaw(const char* fmt, ...);

// Print with the [ReadyUp] log prefix (kLogPrefix).
void Print(const char* fmt, ...);

// Print a single line with the [ReadyUp] log prefix.
void PrintLine(const char* msg);

// Debug-only logging (enabled by `readyup.cfg: debug=1` or env READYUP_DEBUG=1).
// These are intentionally noisy and should be used for deep diagnostics.
void Debug(const char* fmt, ...);
void DebugLine(const char* msg);

}  // namespace readyup

