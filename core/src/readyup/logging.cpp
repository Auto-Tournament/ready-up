#include "readyup/logging.h"

#include "readyup/chat.h"
#include "readyup/config.h"

#include <dlfcn.h>

#include <cstdarg>
#include <cstdio>
#include <string>

namespace readyup {
namespace {

using MsgFn = void (*)(const char* fmt, ...);

// Re-entrancy guard:
// Calling tier0::Msg() from inside engine logging callbacks can re-enter the logging system.
// If that happens, we fall back to stderr to avoid infinite recursion / stack overflow.
static thread_local int g_printDepth = 0;

MsgFn Tier0Msg() {
  static MsgFn fn = nullptr;
  static bool lookedUp = false;
  if (!lookedUp) {
    lookedUp = true;
    fn = reinterpret_cast<MsgFn>(dlsym(RTLD_DEFAULT, "Msg"));
  }
  return fn;
}

// Formats into a string of any length (plugin log lines can carry a JSON dump).
std::string VFormat(const char* fmt, va_list ap) {
  char buf[2048];
  va_list ap2;
  va_copy(ap2, ap);
  const int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
  if (n < 0) {
    va_end(ap2);
    return {};
  }
  if (static_cast<size_t>(n) < sizeof(buf)) {
    va_end(ap2);
    return std::string(buf, static_cast<size_t>(n));
  }
  std::string out(static_cast<size_t>(n) + 1, '\0');
  std::vsnprintf(&out[0], out.size(), fmt, ap2);
  va_end(ap2);
  out.resize(static_cast<size_t>(n));
  return out;
}

// tier0 Msg formats into a fixed buffer too: long text goes out in pieces (no newline added in
// between, so it stays one console line).
void Emit(MsgFn msgFn, const char* prefix, const std::string& s) {
  if (prefix) msgFn("%s ", prefix);
  for (size_t i = 0; i < s.size(); i += 1000) msgFn("%s", s.substr(i, 1000).c_str());
}

void VPrintImpl(bool prefixed, const char* fmt, va_list ap) {
  const std::string s = VFormat(fmt, ap);

  auto msgFn = Tier0Msg();
  if (msgFn && g_printDepth == 0) {
    ++g_printDepth;
    if (s.size() < 1800) {
      if (prefixed) msgFn("%s %s", kLogPrefix, s.c_str());
      else msgFn("%s", s.c_str());
    } else {
      Emit(msgFn, prefixed ? kLogPrefix : nullptr, s);
    }
    --g_printDepth;
  } else {
    if (prefixed) {
      std::fprintf(stderr, "%s %s", kLogPrefix, s.c_str());
    } else {
      std::fprintf(stderr, "%s", s.c_str());
    }
  }

  // Intentionally do NOT mirror Ready Up logs into in-game chat.
  // Mirroring causes "Console:" spam and duplicate lines because many Ready Up
  // features already explicitly send responses to chat (e.g. `.ru` commands).
}

}  // namespace

void PrintRaw(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  VPrintImpl(false, fmt, ap);
  va_end(ap);
}

void Print(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  VPrintImpl(true, fmt, ap);
  va_end(ap);
}

void PrintLine(const char* msg) {
  if (!msg) msg = "(null)";
  auto msgFn = Tier0Msg();
  if (msgFn && g_printDepth == 0) {
    ++g_printDepth;
    msgFn("%s %s\n", kLogPrefix, msg);
    --g_printDepth;
  } else {
    std::fprintf(stderr, "%s %s\n", kLogPrefix, msg);
  }
}

void Debug(const char* fmt, ...) {
  if (!DebugEnabled()) return;
  va_list ap;
  va_start(ap, fmt);
  char buf[2048];
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  auto msgFn = Tier0Msg();
  if (msgFn && g_printDepth == 0) {
    ++g_printDepth;
    msgFn("%s [dbg] %s", kLogPrefix, buf);
    --g_printDepth;
  } else {
    std::fprintf(stderr, "%s [dbg] %s", kLogPrefix, buf);
  }
}

void DebugLine(const char* msg) {
  if (!DebugEnabled()) return;
  if (!msg) msg = "(null)";
  auto msgFn = Tier0Msg();
  if (msgFn && g_printDepth == 0) {
    ++g_printDepth;
    msgFn("%s [dbg] %s\n", kLogPrefix, msg);
    --g_printDepth;
  } else {
    std::fprintf(stderr, "%s [dbg] %s\n", kLogPrefix, msg);
  }
}

}  // namespace readyup

