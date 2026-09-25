#include "readyup/signature_scan.h"

#include "readyup/config.h"
#include "readyup/logging.h"
#include "readyup/path.h"
#include "readyup/real_server.h"

#include <dlfcn.h>
#include <link.h>

#include <cctype>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace readyup {
namespace {

static bool IsHex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int HexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return 0;
}

static std::vector<int> ParsePattern(const std::string& pattern) {
  std::vector<int> out;
  out.reserve(pattern.size() / 2);

  for (size_t i = 0; i < pattern.size();) {
    while (i < pattern.size() && std::isspace(static_cast<unsigned char>(pattern[i])) != 0) ++i;
    if (i >= pattern.size()) break;

    if (pattern[i] == '?') {
      out.push_back(-1);
      ++i;
      if (i < pattern.size() && pattern[i] == '?') ++i;
      continue;
    }

    if (i + 1 < pattern.size() && IsHex(pattern[i]) && IsHex(pattern[i + 1])) {
      const int v = (HexVal(pattern[i]) << 4) | HexVal(pattern[i + 1]);
      out.push_back(v);
      i += 2;
      continue;
    }

    // Unknown token; skip.
    ++i;
  }

  return out;
}

// Which loaded object is Valve's server library? Primarily the one we dlopen()ed ourselves
// (real_server.cpp): its link_map load address is compared with each dl_iterate_phdr entry.
// Matching by file name alone is not enough: under Metamod the process also contains
// csgo/addons/metamod/bin/linuxsteamrt64/libserver.so, loaded before Valve's, and scanning
// that one made every signature miss and disabled Ready Up (docs/COMPATIBILITY.md).
static bool RealServerLoadBase(uintptr_t* out) {
  static std::once_flag once;
  static bool ok = false;
  static uintptr_t base = 0;
  std::call_once(once, [] {
    void* h = RealServerHandle();
    if (!h) return;
    struct link_map* lm = nullptr;
    if (dlinfo(h, RTLD_DI_LINKMAP, &lm) != 0 || !lm) return;
    base = static_cast<uintptr_t>(lm->l_addr);
    ok = true;
  });
  if (ok && out) *out = base;
  return ok;
}

static bool IsRealValveServerModule(const struct dl_phdr_info* info) {
  if (!info) return false;
  uintptr_t base = 0;
  if (RealServerLoadBase(&base)) return static_cast<uintptr_t>(info->dlpi_addr) == base;
  // No handle (the real module failed to load): name-based fallback, which skips our shim and
  // Metamod's proxy library.
  return info->dlpi_name && LooksLikeValveServerModule(info->dlpi_name);
}

static void* ScanRegion(const uint8_t* base, size_t size, const std::vector<int>& pat) {
  if (!base || size == 0 || pat.empty()) return nullptr;
  if (size < pat.size()) return nullptr;

  for (size_t i = 0; i + pat.size() <= size; ++i) {
    bool ok = true;
    for (size_t j = 0; j < pat.size(); ++j) {
      const int want = pat[j];
      if (want < 0) continue;
      if (base[i + j] != static_cast<uint8_t>(want)) {
        ok = false;
        break;
      }
    }
    if (ok) return const_cast<uint8_t*>(base + i);
  }
  return nullptr;
}

static SigResult ScanRegionCount(const uint8_t* base, size_t size, const std::vector<int>& pat, int maxMatches) {
  SigResult r;
  r.pat_len = pat.size();
  if (!base || size == 0 || pat.empty()) return r;
  if (size < pat.size()) return r;
  if (maxMatches <= 0) maxMatches = 1;

  for (size_t i = 0; i + pat.size() <= size; ++i) {
    bool ok = true;
    for (size_t j = 0; j < pat.size(); ++j) {
      const int want = pat[j];
      if (want < 0) continue;
      if (base[i + j] != static_cast<uint8_t>(want)) {
        ok = false;
        break;
      }
    }
    if (!ok) continue;

    if (r.matches == 0) r.addr = const_cast<uint8_t*>(base + i);
    ++r.matches;
    if (r.matches >= maxMatches) return r;

    // Skip forward a bit to avoid O(n*m) on repeated patterns.
    i += pat.size() - 1;
  }
  return r;
}

struct FindCtx {
  std::vector<int> pat;
  void* found = nullptr;
};

static int IterateCb(struct dl_phdr_info* info, size_t, void* data) {
  auto* ctx = reinterpret_cast<FindCtx*>(data);
  if (!ctx || ctx->found) return 1;

  if (!IsRealValveServerModule(info)) return 0;

  for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
    const ElfW(Phdr)& ph = info->dlpi_phdr[i];
    if (ph.p_type != PT_LOAD) continue;
    if ((ph.p_flags & PF_X) == 0) continue;  // executable only

    const auto* seg = reinterpret_cast<const uint8_t*>(info->dlpi_addr + ph.p_vaddr);
    const size_t segsz = static_cast<size_t>(ph.p_memsz);
    void* hit = ScanRegion(seg, segsz, ctx->pat);
    if (hit) {
      ctx->found = hit;
      return 1;
    }
  }

  return 0;
}

}  // namespace

void* FindInRealServerText(const std::string& pattern) {
  RealServerLoadBase(nullptr);  // resolve outside the dl_iterate_phdr callback (loader lock)
  FindCtx ctx;
  ctx.pat = ParsePattern(pattern);
  if (ctx.pat.empty()) return nullptr;

  dl_iterate_phdr(IterateCb, &ctx);
  if (DebugEnabled()) {
    Print("sigscan: pattern len=%zu found=%p\n", ctx.pat.size(), ctx.found);
  }
  return ctx.found;
}

SigResult FindInRealServerTextCount(const std::string& pattern, int maxMatches) {
  RealServerLoadBase(nullptr);  // resolve outside the dl_iterate_phdr callback (loader lock)
  SigResult out;
  const auto pat = ParsePattern(pattern);
  out.pat_len = pat.size();
  if (pat.empty()) return out;

  struct CountCtx {
    std::vector<int> pat;
    SigResult res;
    int maxMatches = 3;
  } ctx;
  ctx.pat = pat;
  ctx.res.pat_len = pat.size();
  ctx.maxMatches = maxMatches;

  auto cb = [](struct dl_phdr_info* info, size_t, void* data) -> int {
    auto* c = reinterpret_cast<CountCtx*>(data);
    if (!c) return 0;
    if (c->res.matches >= c->maxMatches) return 1;
    if (!IsRealValveServerModule(info)) return 0;

    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
      const ElfW(Phdr)& ph = info->dlpi_phdr[i];
      if (ph.p_type != PT_LOAD) continue;
      if ((ph.p_flags & PF_X) == 0) continue;
      const auto* seg = reinterpret_cast<const uint8_t*>(info->dlpi_addr + ph.p_vaddr);
      const size_t segsz = static_cast<size_t>(ph.p_memsz);
      SigResult r = ScanRegionCount(seg, segsz, c->pat, c->maxMatches - c->res.matches);
      if (r.matches > 0 && c->res.matches == 0) c->res.addr = r.addr;
      c->res.matches += r.matches;
      if (c->res.matches >= c->maxMatches) return 1;
    }
    return 0;
  };

  dl_iterate_phdr(cb, &ctx);
  out.addr = ctx.res.addr;
  out.matches = ctx.res.matches;
  out.pat_len = ctx.res.pat_len;

  if (DebugEnabled()) {
    Print("sigscan: pattern len=%zu matches=%d first=%p\n", out.pat_len, out.matches, out.addr);
  }
  return out;
}

es::Image SnapshotRealServerImage() {
  RealServerLoadBase(nullptr);  // resolve outside the dl_iterate_phdr callback (loader lock)
  es::Image img;
  auto cb = [](struct dl_phdr_info* info, size_t, void* data) -> int {
    auto* out = reinterpret_cast<es::Image*>(data);
    if (!IsRealValveServerModule(info)) return 0;
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
      const ElfW(Phdr)& ph = info->dlpi_phdr[i];
      if (ph.p_type != PT_LOAD) continue;
      es::Region r;
      r.addr = static_cast<uintptr_t>(info->dlpi_addr + ph.p_vaddr);
      r.data = reinterpret_cast<const uint8_t*>(r.addr);
      r.size = static_cast<size_t>(ph.p_memsz);
      r.exec = (ph.p_flags & PF_X) != 0;
      out->regions.push_back(r);
    }
    return 1;  // first real server module only
  };
  dl_iterate_phdr(cb, &img);
  return img;
}

}  // namespace readyup
