#include "readyup/command_buffer_hook.h"

#include "readyup/config.h"
#include "readyup/logging.h"
#include "readyup/plugin_loader.h"
#include "readyup/ru_help.h"
#include "readyup/ru_router.h"
#include "readyup/selftest.h"
#include "readyup/sigtest.h"
#include "readyup/status_feed.h"
#include "readyup/version.h"

#include <dlfcn.h>
#include <link.h>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace readyup {
namespace {

// Mangled name for: CCommandBuffer::AddText(const char*, int, int, bool, double, uint64_t)
static constexpr const char* kAddTextSym = "_ZN14CCommandBuffer7AddTextEPKciibdy";

using AddTextFn = void (*)(void* thisptr, const char* text, int a, int b, bool c, double d, uint64_t e);
AddTextFn g_orig = nullptr;

std::atomic<bool> g_installed{false};

struct LastAddTextCall {
  void* thisptr = nullptr;
  int a = 0;
  int b = 0;
  bool c = false;
  double d = 0.0;
  uint64_t e = 0;
};

std::atomic<void*> g_lastThis{nullptr};
// Remaining params are updated without atomics; benign races are fine (best-effort).
LastAddTextCall g_last{};

static std::string Trim(std::string s) {
  auto is_ws = [](unsigned char ch) { return std::isspace(ch) != 0; };
  while (!s.empty() && is_ws(static_cast<unsigned char>(s.back()))) s.pop_back();
  size_t i = 0;
  while (i < s.size() && is_ws(static_cast<unsigned char>(s[i]))) ++i;
  if (i) s.erase(0, i);
  return s;
}

static std::vector<std::string> SplitWS(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (size_t i = 0; i < s.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (std::isspace(c) != 0) {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
      continue;
    }
    cur.push_back(static_cast<char>(c));
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

static bool StartsWithToken(const std::string& s, const char* tok) {
  const size_t n = std::strlen(tok);
  if (s.size() < n) return false;
  if (std::memcmp(s.data(), tok, n) != 0) return false;
  if (s.size() == n) return true;
  const unsigned char next = static_cast<unsigned char>(s[n]);
  return std::isspace(next) != 0 || next == ';' || next == '\n' || next == '\r';
}

static bool HandleRuCommandLine(const std::string& line) {
  // Accept `ru ...` from server console (not `.ru`).
  std::string t = Trim(line);
  if (t.empty()) return false;
  if (!StartsWithToken(t, "ru")) return false;

  auto parts = SplitWS(t);
  if (parts.empty()) return false;

  // `ru` / `ru help`. Every other `ru <sub>` (match load, idle, state, start, ...) belongs to a
  // plugin (register_ru_subcommand; readyup-match), or is unknown.
  if (parts.size() == 1 || (parts.size() >= 2 && parts[1] == "help")) {
    PrintRuHelp();
    return true;
  }

  if (parts[1] == "plugin" || parts[1] == "plugins") {
    const std::vector<std::string> args(parts.begin() + 2, parts.end());
    plugins::HandlePluginCommand(args, /*replyToChat=*/false);
    return true;
  }

  if (parts[1] == "sigtest") {
    const bool ok = RunSigTest(/*verbose=*/true);
    PrintLine(ok ? "sigtest OK" : "sigtest FAIL");
    return true;
  }

  if (parts[1] == "selftest") {
    (void)RunSelftest(/*printToConsole=*/true);
    return true;
  }

  if (parts[1] == "status_http") {
    for (const auto& l : status_feed::StatusLines()) Print("%s\n", l.c_str());
    return true;
  }

  if (parts[1] == "reload") {
    std::string err;
    if (!ReloadCfg(&err)) {
      PrintLine(err.empty() ? "reload: failed" : ("reload: failed: " + err).c_str());
      return true;
    }
    PrintLine("reload: ok");
    return true;
  }

  // `ru <sub>` a plugin registered (register_ru_subcommand); runs on the next GameFrame.
  if (plugins::TryDispatchRu(/*console=*/true, 0, "Console", t)) return true;

  // `ru <cmd> ...` reaches a console command a plugin registered, so plugins can offer
  // `ru fleet status` next to the core's own `ru ...` commands (runs on the next GameFrame).
  {
    std::string rest;
    for (size_t i = 1; i < parts.size(); ++i) rest += (i > 1 ? " " : "") + parts[i];
    if (plugins::TryDispatchConsole(rest)) return true;
  }

  if (parts[1] == "version") {
    Print("Ready Up %s\n", BuildVersion());
    return true;
  }

  PrintLine("Unknown Ready Up command. Try: ru help");
  return true;
}

static bool HandleReadyUpConsoleCommandLine(const std::string& line) {
  // RU_CMD_OBSERVE console registrations see the line; it still runs normally.
  plugins::ObserveConsole(Trim(line));

  // Handle `ru ...` command family.
  if (HandleRuCommandLine(line)) return true;

  // Console commands registered by plugins (core commands above always win). The
  // plugin callback runs on the next GameFrame, never inside AddText.
  return plugins::TryDispatchConsole(Trim(line));
}

static void Hook_AddText(void* thisptr, const char* text, int a, int b, bool c, double d, uint64_t e) {
  // Capture last known command buffer invocation so we can enqueue commands later.
  g_last.thisptr = thisptr;
  g_last.a = a;
  g_last.b = b;
  g_last.c = c;
  g_last.d = d;
  g_last.e = e;
  g_lastThis.store(thisptr, std::memory_order_release);

  // Intercept only simple single-line `ru ...` commands.
  // We intentionally do NOT try to parse complex command buffers.
  if (text) {
    const std::string s(text);
    // Use first line only.
    const size_t nl = s.find_first_of("\r\n");
    const std::string first = (nl == std::string::npos) ? s : s.substr(0, nl);
    if (HandleReadyUpConsoleCommandLine(first)) {
      // Swallow this command so the engine doesn't try to interpret it as an alias/unknown.
      return;
    }
  }

  if (g_orig) g_orig(thisptr, text, a, b, c, d, e);
}

// Minimal ELF PLT/GOT patcher for the current process.
struct ElfDynInfo {
  const ElfW(Sym)* symtab = nullptr;
  const char* strtab = nullptr;
  size_t syment = 0;
  const ElfW(Rela)* jmprel_rela = nullptr;
  size_t pltrelsz = 0;
  const ElfW(Rel)* jmprel_rel = nullptr;
  bool rela = true;
};

static bool ReadDynInfo(struct dl_phdr_info* info, ElfDynInfo& out) {
  const ElfW(Phdr)* phdr = info->dlpi_phdr;
  for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
    if (phdr[i].p_type != PT_DYNAMIC) continue;
    const ElfW(Dyn)* dyn = reinterpret_cast<const ElfW(Dyn)*>(info->dlpi_addr + phdr[i].p_vaddr);

    ElfW(Addr) symtab = 0;
    ElfW(Addr) strtab = 0;
    ElfW(Addr) jmprel = 0;
    size_t pltrelsz = 0;
    size_t syment = 0;
    ElfW(Sword) pltrel = DT_RELA;

    for (const ElfW(Dyn)* d = dyn; d->d_tag != DT_NULL; ++d) {
      switch (d->d_tag) {
        case DT_SYMTAB: symtab = d->d_un.d_ptr; break;
        case DT_STRTAB: strtab = d->d_un.d_ptr; break;
        case DT_JMPREL: jmprel = d->d_un.d_ptr; break;
        case DT_PLTRELSZ: pltrelsz = static_cast<size_t>(d->d_un.d_val); break;
        case DT_SYMENT: syment = static_cast<size_t>(d->d_un.d_val); break;
        case DT_PLTREL: pltrel = d->d_un.d_val; break;
        default: break;
      }
    }

    if (!symtab || !strtab || !jmprel || !pltrelsz) return false;

    out.symtab = reinterpret_cast<const ElfW(Sym)*>(symtab);
    out.strtab = reinterpret_cast<const char*>(strtab);
    out.syment = syment;
    out.pltrelsz = pltrelsz;
    out.rela = (pltrel == DT_RELA);
    if (out.rela) out.jmprel_rela = reinterpret_cast<const ElfW(Rela)*>(jmprel);
    else out.jmprel_rel = reinterpret_cast<const ElfW(Rel)*>(jmprel);
    return true;
  }
  return false;
}

static bool PatchGotEntry(void** got, void* replacement, void** outOld) {
  if (!got || !replacement) return false;
  const uintptr_t addr = reinterpret_cast<uintptr_t>(got);
  const uintptr_t page = addr & ~(static_cast<uintptr_t>(getpagesize() - 1));
  if (mprotect(reinterpret_cast<void*>(page), static_cast<size_t>(getpagesize()), PROT_READ | PROT_WRITE) != 0) {
    return false;
  }
  if (outOld) *outOld = *got;
  *got = replacement;
  // Restore to RX/R (best-effort).
  mprotect(reinterpret_cast<void*>(page), static_cast<size_t>(getpagesize()), PROT_READ);
  return true;
}

static int PatchModule(struct dl_phdr_info* info, size_t, void* /*data*/) {
  // Skip the main executable name empty? still patch.
  ElfDynInfo dyn{};
  if (!ReadDynInfo(info, dyn)) return 0;

  auto patchOne = [&](ElfW(Addr) r_offset, unsigned long symIndex) {
    const ElfW(Sym)& sym = dyn.symtab[symIndex];
    const char* name = dyn.strtab + sym.st_name;
    if (!name) return;
    if (std::strcmp(name, kAddTextSym) != 0) return;

    void** got = reinterpret_cast<void**>(info->dlpi_addr + r_offset);
    void* old = nullptr;
    if (PatchGotEntry(got, reinterpret_cast<void*>(Hook_AddText), &old)) {
      if (!g_orig) g_orig = reinterpret_cast<AddTextFn>(old);
      if (DebugEnabled()) {
        Print("console: hooked CCommandBuffer::AddText in module: %s\n", info->dlpi_name && *info->dlpi_name ? info->dlpi_name : "(main)");
      }
    }
  };

  if (dyn.rela && dyn.jmprel_rela) {
    const size_t n = dyn.pltrelsz / sizeof(ElfW(Rela));
    for (size_t i = 0; i < n; ++i) {
      const ElfW(Rela)& r = dyn.jmprel_rela[i];
      const unsigned long symIndex = ELF64_R_SYM(r.r_info);
      patchOne(r.r_offset, symIndex);
    }
  } else if (!dyn.rela && dyn.jmprel_rel) {
    const size_t n = dyn.pltrelsz / sizeof(ElfW(Rel));
    for (size_t i = 0; i < n; ++i) {
      const ElfW(Rel)& r = dyn.jmprel_rel[i];
      const unsigned long symIndex = ELF64_R_SYM(r.r_info);
      patchOne(r.r_offset, symIndex);
    }
  }

  return 0;
}

}  // namespace

void InstallCommandBufferHook() {
  bool expected = false;
  if (!g_installed.compare_exchange_strong(expected, true)) return;

  dl_iterate_phdr(PatchModule, nullptr);

  if (!g_orig && DebugEnabled()) {
    PrintLine("console: failed to hook CCommandBuffer::AddText (console `ru` commands disabled).");
  }
}

bool EnqueueServerCommand(const char* text) {
  if (!text || !*text) return false;
  void* thisptr = g_lastThis.load(std::memory_order_acquire);
  if (!thisptr || !g_orig) return false;

  // Ensure newline termination (most engine command buffer users expect it).
  std::string s(text);
  if (s.back() != '\n') s.push_back('\n');

  // Reuse the latest parameter values the engine called AddText with.
  // This keeps us ABI-stable without guessing flag meanings.
  g_orig(thisptr, s.c_str(), g_last.a, g_last.b, g_last.c, g_last.d, g_last.e);
  return true;
}

}  // namespace readyup


namespace readyup {

bool CommandBufferHookInstalled() { return g_orig != nullptr; }

bool CommandBufferSeen() { return g_lastThis.load(std::memory_order_acquire) != nullptr; }

}  // namespace readyup
