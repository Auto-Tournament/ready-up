#pragma once

// Console logging for readyup-match (host.cpp): `[ReadyUp] <msg>` through ru_api log_untagged,
// so the lines read exactly as they did when the match flow was part of the core (tools and
// scripts/livetest parse `state:`, `knife:`, `match-load[..]:`). Any thread.

namespace readyup {

constexpr const char* kLogPrefix = "[ReadyUp]";

// Print with the [ReadyUp] log prefix. One call = one console line (long lines are kept whole).
void Print(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void PrintLine(const char* msg);
// Only when readyup.cfg debug=1 (or READYUP_DEBUG=1): `[ReadyUp] [dbg] <msg>`.
void Debug(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void DebugLine(const char* msg);

}  // namespace readyup
