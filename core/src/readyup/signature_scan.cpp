#include "readyup/signature_scan.h"

#include "readyup/config.h"
#include "readyup/logging.h"

#include <link.h>

#include <cctype>
#include <cstdint>
#include <cstring>
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

static std::string CanonicalizePath(std::string s) {
  // Purely lexical canonicalization: collapses `/./` and `/../` segments.
  // This is important because dlpi_name may include `..` (e.g. when dlopen() used a relative path),
  // and our module filters rely on substring checks.
  const bool abs = !s.empty() && s[0] == '/';

  std::vector<std::string> parts;
  parts.reserve(32);

  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && s[i] == '/') ++i;
    if (i >= s.size()) break;
    size_t j = i;
    while (j < s.size() && s[j] != '/') ++j;
    std::string seg = s.substr(i, j - i);
    i = j;

    if (seg.empty() || seg == ".") continue;
    if (seg == "..") {
      if (!parts.empty() && parts.back() != "..") {
        parts.pop_back();
      } else if (!abs) {
        parts.push_back("..");
      }
      continue;
    }
    parts.push_back(std::move(seg));
  }

  std::string out;
  if (abs) out.push_back('/');
  for (size_t k = 0; k < parts.size(); ++k) {
    if (k != 0) out.push_back('/');
    out.append(parts[k]);
  }
  if (out.empty()) out = abs ? "/" : ".";
  return out;
}

static bool IsRealValveServerModuleName(const char* name) {
  if (!name || !*name) return false;
  const std::string s = CanonicalizePath(std::string(name));
  if (s.find("/bin/linuxsteamrt64/libserver.so") == std::string::npos) return false;
  // Exclude our shim: typically lives under /csgo/<gamepath>/.../readyup/.../libserver.so.
  // We canonicalize first so paths like ".../readyup/.../../../../bin/.../libserver.so"
  // don't get incorrectly rejected.
  if (s.find("/csgo/") != std::string::npos &&
      s.find("/readyup/bin/linuxsteamrt64/libserver.so") != std::string::npos) {
    return false;
  }
  return true;
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

  if (!IsRealValveServerModuleName(info->dlpi_name)) return 0;

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
    if (!IsRealValveServerModuleName(info->dlpi_name)) return 0;

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
  es::Image img;
  auto cb = [](struct dl_phdr_info* info, size_t, void* data) -> int {
    auto* out = reinterpret_cast<es::Image*>(data);
    if (!IsRealValveServerModuleName(info->dlpi_name)) return 0;
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
