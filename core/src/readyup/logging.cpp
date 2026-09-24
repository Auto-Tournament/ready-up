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

void VPrintImpl(bool prefixed, const char* fmt, va_list ap) {
  char buf[2048];
  std::vsnprintf(buf, sizeof(buf), fmt, ap);

  auto msgFn = Tier0Msg();
  if (msgFn && g_printDepth == 0) {
    ++g_printDepth;
    if (prefixed) {
      msgFn("%s %s", kLogPrefix, buf);
    } else {
      msgFn("%s", buf);
    }
    --g_printDepth;
  } else {
    if (prefixed) {
      std::fprintf(stderr, "%s %s", kLogPrefix, buf);
    } else {
      std::fprintf(stderr, "%s", buf);
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

