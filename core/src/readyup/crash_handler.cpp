#include "readyup/crash_handler.h"

#include "readyup/logging.h"
#include "readyup/plugin_loader.h"
#include "readyup/path.h"
#include "readyup/version.h"

#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace readyup {
namespace {

// Everything the handler needs is prepared at install time: the handler itself only uses
// async-signal-safe calls (write, open, close, alarm, sigaction, raise, backtrace_symbols_fd) plus
// backtrace(), whose one-time libgcc_s load is forced at install (see below).
char g_crashLogPath[1024] = {0};
volatile sig_atomic_t g_inHandler = 0;

constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};

const char* SigName(int sig) {
  switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    case SIGABRT: return "SIGABRT";
    default: return "SIGNAL";
  }
}

void WriteAll(int fd, const char* s, size_t n) {
  while (fd >= 0 && n > 0) {
    const ssize_t w = write(fd, s, n);
    if (w <= 0) return;
    s += w;
    n -= static_cast<size_t>(w);
  }
}

void WriteStr(int fd, const char* s) { WriteAll(fd, s, std::strlen(s)); }

// Minimal async-signal-safe integer/pointer formatting (no snprintf in the handler).
void WriteHex(int fd, uintptr_t v) {
  char buf[2 + 16 + 1];
  char* p = buf + sizeof(buf);
  *--p = '\0';
  do {
    *--p = "0123456789abcdef"[v & 0xF];
    v >>= 4;
  } while (v && p > buf + 2);
  *--p = 'x';
  *--p = '0';
  WriteStr(fd, p);
}

void WriteDec(int fd, long v) {
  char buf[24];
  char* p = buf + sizeof(buf);
  *--p = '\0';
  const bool neg = v < 0;
  unsigned long u = neg ? static_cast<unsigned long>(-v) : static_cast<unsigned long>(v);
  do {
    *--p = static_cast<char>('0' + (u % 10));
    u /= 10;
  } while (u && p > buf + 1);
  if (neg) *--p = '-';
  WriteStr(fd, p);
}

// The process's mappings, read at crash time (open/read are async-signal-safe), to name each
// code address as module+offset: glibc's backtrace() stops at the signal frame when the faulting
// code has no unwind info, and the four hud.so crashes of 2026-11-24 logged only two frames.
char g_maps[512 * 1024];
struct Mapping {
  uintptr_t lo, hi, off;
  const char* name;  // the file's basename inside g_maps (not NUL-terminated)
  int nameLen;
  bool exec;
};
Mapping g_map[4096];
int g_nMap = 0;

uintptr_t ParseHex(const char*& p, const char* end) {
  uintptr_t v = 0;
  for (; p < end; ++p) {
    const char c = *p;
    int d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else break;
    v = (v << 4) | static_cast<uintptr_t>(d);
  }
  return v;
}

void SkipField(const char*& p, const char* end) {
  while (p < end && *p == ' ') ++p;
  while (p < end && *p != ' ') ++p;
  while (p < end && *p == ' ') ++p;
}

void LoadMaps() {
  g_nMap = 0;
  const int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return;
  size_t n = 0;
  while (n < sizeof(g_maps) - 1) {
    const ssize_t r = read(fd, g_maps + n, sizeof(g_maps) - 1 - n);
    if (r <= 0) break;
    n += static_cast<size_t>(r);
  }
  close(fd);
  g_maps[n] = '\0';
  // "lo-hi perms offset dev inode   path"
  const char* line = g_maps;
  const char* const all = g_maps + n;
  while (line < all && g_nMap < static_cast<int>(sizeof(g_map) / sizeof(g_map[0]))) {
    const char* end = line;
    while (end < all && *end != '\n') ++end;
    const char* p = line;
    Mapping m{};
    m.lo = ParseHex(p, end);
    if (p < end && *p == '-') ++p;
    m.hi = ParseHex(p, end);
    while (p < end && *p == ' ') ++p;
    m.exec = end - p > 3 && p[2] == 'x';
    SkipField(p, end);  // perms
    m.off = ParseHex(p, end);
    SkipField(p, end);  // the offset's rest (none) and dev
    SkipField(p, end);  // inode
    if (p < end) {
      const char* base = p;
      for (const char* q = p; q < end; ++q)
        if (*q == '/') base = q + 1;
      m.name = base;
      m.nameLen = static_cast<int>(end - base);
    }
    if (m.hi > m.lo) g_map[g_nMap++] = m;
    line = end < all ? end + 1 : end;
  }
}

const Mapping* MapOf(uintptr_t a) {
  for (int i = 0; i < g_nMap; ++i)
    if (a >= g_map[i].lo && a < g_map[i].hi) return &g_map[i];
  return nullptr;
}

// "0x7f..  (hud.so+0x1234)" for an address in a named mapping; just the address otherwise.
void WriteWhere(int fd, uintptr_t a) {
  WriteHex(fd, a);
  const Mapping* m = MapOf(a);
  if (!m || !m->name || m->nameLen <= 0) return;
  WriteStr(fd, " (");
  WriteAll(fd, m->name, static_cast<size_t>(m->nameLen));
  WriteStr(fd, "+");
  WriteHex(fd, a - m->lo + m->off);
  WriteStr(fd, ")");
}

// rip, and every stack word that points into code of a named module (most are return addresses;
// a few are stale): enough for addr2line on the release's .so files.
void ReportContext(int fd, const ucontext_t* uc) {
  if (!uc) return;
#if defined(__x86_64__)
  const uintptr_t rip = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
  const uintptr_t rsp = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RSP]);
  WriteStr(fd, "[ReadyUp] rip=");
  WriteWhere(fd, rip);
  WriteStr(fd, " rsp=");
  WriteHex(fd, rsp);
  WriteStr(fd, "\n");
  const Mapping* stack = MapOf(rsp);
  if (!stack) return;
  WriteStr(fd, "[ReadyUp] code addresses on the stack:\n");
  const uintptr_t top = rsp + 64 * 1024 < stack->hi ? rsp + 64 * 1024 : stack->hi;
  int shown = 0;
  for (uintptr_t a = rsp & ~static_cast<uintptr_t>(7); a + 8 <= top && shown < 48; a += 8) {
    const uintptr_t v = *reinterpret_cast<const uintptr_t*>(a);
    const Mapping* m = MapOf(v);
    if (!m || !m->exec || !m->name || m->nameLen <= 0) continue;
    WriteStr(fd, "[ReadyUp]   ");
    WriteWhere(fd, v);
    WriteStr(fd, "\n");
    ++shown;
  }
#else
  (void)fd;
#endif
}

void Report(int fd, int sig, siginfo_t* info, void** addrs, int n, const ucontext_t* uc) {
  if (fd < 0) return;
  WriteStr(fd, "\n[ReadyUp] === crash handler (Ready Up " READYUP_BUILD_VERSION ") ===\n[ReadyUp] signal=");
  WriteStr(fd, SigName(sig));
  WriteStr(fd, " (");
  WriteDec(fd, sig);
  WriteStr(fd, ") addr=");
  WriteHex(fd, reinterpret_cast<uintptr_t>(info ? info->si_addr : nullptr));
  WriteStr(fd, " code=");
  WriteDec(fd, info ? info->si_code : 0);
  WriteStr(fd, " pid=");
  WriteDec(fd, static_cast<long>(getpid()));
  WriteStr(fd, "\n");
  // A plugin callback on the stack is the first suspect.
  const char* plugin = plugins::CrashContextPlugin();
  if (plugin && *plugin) {
    WriteStr(fd, "[ReadyUp] crashed inside plugin \"");
    WriteStr(fd, plugin);
    WriteStr(fd, "\"\n");
  }
  ReportContext(fd, uc);
  if (n > 0) {
    backtrace_symbols_fd(addrs, n, fd);
  } else {
    WriteStr(fd, "[ReadyUp] (backtrace() returned 0)\n");
  }
  WriteStr(fd, "[ReadyUp] === end crash handler; re-raising with the default action ===\n");
}

void CrashHandler(int sig, siginfo_t* info, void* uctx) {
  // Never let a crash inside the handler (or a second thread crashing concurrently) recurse.
  if (g_inHandler) {
    signal(sig, SIG_DFL);
    raise(sig);
    _exit(128 + sig);
  }
  g_inHandler = 1;

  // Backstop: if anything below deadlocks (e.g. the crash happened inside malloc while backtrace
  // needs a lock), SIGALRM's default action terminates the process in 10 s instead of hanging.
  signal(SIGALRM, SIG_DFL);
  alarm(10);

  void* addrs[96];
  const int n = backtrace(addrs, static_cast<int>(sizeof(addrs) / sizeof(addrs[0])));

  LoadMaps();
  const auto* uc = static_cast<const ucontext_t*>(uctx);
  Report(STDERR_FILENO, sig, info, addrs, n, uc);
  if (g_crashLogPath[0]) {
    const int fd = open(g_crashLogPath, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd >= 0) {
      Report(fd, sig, info, addrs, n, uc);
      close(fd);
    }
  }

  // Restore the default action and re-raise: the process dies with the original signal (core
  // dump if enabled) so the supervisor sees a dead server instead of a hung one.
  struct sigaction dfl;
  std::memset(&dfl, 0, sizeof(dfl));
  dfl.sa_handler = SIG_DFL;
  sigemptyset(&dfl.sa_mask);
  sigaction(sig, &dfl, nullptr);
  sigset_t unblock;
  sigemptyset(&unblock);
  sigaddset(&unblock, sig);
  sigprocmask(SIG_UNBLOCK, &unblock, nullptr);
  raise(sig);
  // Synchronous faults (SIGSEGV etc.) re-trigger on return anyway; this covers the rest.
  _exit(128 + sig);
}

bool OptedOut() {
  const char* v = std::getenv("READYUP_CRASH_HANDLER");
  return v && (v[0] == '0' || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F');
}

}  // namespace

void InstallCrashHandlersMaybe() {
  static std::once_flag once;
  std::call_once(once, []() {
    if (OptedOut()) {
      PrintLine("crash handler disabled (READYUP_CRASH_HANDLER=0).");
      return;
    }
    const std::string dir = GetThisModuleDir();
    const std::string path = (dir.empty() ? std::string(".") : dir) + "/readyup_crash.log";
    std::snprintf(g_crashLogPath, sizeof(g_crashLogPath), "%s", path.c_str());

    // backtrace() dlopens libgcc_s on first use (malloc + loader locks): do it now, not in the
    // handler, so the handler cannot deadlock on those locks.
    void* warm[4];
    (void)backtrace(warm, 4);

    // Run on an alternate stack so a stack overflow can still be reported.
    static char altStack[64 * 1024];
    stack_t ss;
    std::memset(&ss, 0, sizeof(ss));
    ss.ss_sp = altStack;
    ss.ss_size = sizeof(altStack);
    (void)sigaltstack(&ss, nullptr);

    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = CrashHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    for (int sig : kSignals) sigaction(sig, &sa, nullptr);

    Print("crash handler installed (backtrace -> stderr + %s; then default action).\n", g_crashLogPath);
  });
}

}  // namespace readyup
