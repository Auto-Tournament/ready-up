#include "readyup/crash_handler.h"

#include "readyup/logging.h"
#include "readyup/plugin_loader.h"
#include "readyup/path.h"
#include "readyup/version.h"

#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
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

void Report(int fd, int sig, siginfo_t* info, void** addrs, int n) {
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
  if (n > 0) {
    backtrace_symbols_fd(addrs, n, fd);
  } else {
    WriteStr(fd, "[ReadyUp] (backtrace() returned 0)\n");
  }
  WriteStr(fd, "[ReadyUp] === end crash handler; re-raising with the default action ===\n");
}

void CrashHandler(int sig, siginfo_t* info, void* /*uctx*/) {
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

  Report(STDERR_FILENO, sig, info, addrs, n);
  if (g_crashLogPath[0]) {
    const int fd = open(g_crashLogPath, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd >= 0) {
      Report(fd, sig, info, addrs, n);
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
