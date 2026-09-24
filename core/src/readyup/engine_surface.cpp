#include "readyup/engine_surface.h"

#include "readyup/logging.h"
#include "readyup/path.h"
#include "readyup/signature_scan.h"

#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

// Generated at build time from gamedata/engine-surface.json (see CMakeLists.txt).
extern const char kReadyUpEmbeddedEngineSurfaceJson[];

namespace readyup {
namespace {

// All state lives in function-local statics: this module is used from the shim's
// load-time constructor, which may run before namespace-scope objects of this TU are
// dynamically initialized (a later initializer would silently wipe them).
struct State {
  std::once_flag loadOnce;
  std::optional<es::EngineSurface> surface;
  std::mutex mu;
  std::map<std::string, es::Resolution> cache;
  std::optional<es::Image> image;
  std::map<std::string, VtableVerdict> vtables;  // object-less slot verification results
};

State& S() {
  static State s;
  return s;
}

void LoadOnce() {
  auto& g_surface = S().surface;
  std::string json;
  std::string source = "embedded";
  const std::string dir = GetThisModuleDir();
  if (!dir.empty()) {
    const std::string path = dir + "/engine-surface.json";
    std::ifstream f(path);
    if (f.good()) {
      std::stringstream ss;
      ss << f.rdbuf();
      json = ss.str();
      source = path;
    }
  }
  if (json.empty()) json = kReadyUpEmbeddedEngineSurfaceJson;

  std::string err;
  g_surface = es::ParseEngineSurface(json, &err);
  if (!g_surface && source != "embedded") {
    Print("engine-surface: failed to parse %s (%s); falling back to embedded copy\n", source.c_str(), err.c_str());
    source = "embedded";
    g_surface = es::ParseEngineSurface(kReadyUpEmbeddedEngineSurfaceJson, &err);
  }
  if (!g_surface) {
    Print("engine-surface: embedded copy failed to parse (%s); all engine functions unresolved\n", err.c_str());
    return;
  }
  Print("engine-surface: loaded %zu functions from %s (built for CS2 %s)\n", g_surface->functions.size(), source.c_str(),
        g_surface->game_version.c_str());
}

// Reads memory without faulting (EFAULT on unmapped addresses).
bool SafeRead(uintptr_t addr, void* out, size_t n) {
  struct iovec local {
    out, n
  };
  struct iovec remote {
    reinterpret_cast<void*>(addr), n
  };
  return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(n);
}

}  // namespace

const es::EngineSurface* GetEngineSurface() {
  std::call_once(S().loadOnce, LoadOnce);
  return S().surface ? &*S().surface : nullptr;
}

const es::Image* RealServerImage() {
  std::lock_guard<std::mutex> lk(S().mu);
  if (!S().image) {
    es::Image img = SnapshotRealServerImage();
    if (img.regions.empty()) return nullptr;  // not loaded yet; retry later
    S().image = std::move(img);
  }
  return &*S().image;
}

es::Resolution EngineFunctionResolution(const char* name) {
  const es::EngineSurface* s = GetEngineSurface();
  es::Resolution res;
  if (!s || !name) {
    res.detail = "engine surface unavailable";
    return res;
  }
  const es::FunctionSpec* spec = s->Find(name);
  if (!spec) {
    res.detail = "not listed in engine-surface.json";
    return res;
  }
  const es::Image* img = RealServerImage();
  if (!img) {
    res.detail = "real libserver.so not loaded";
    return res;
  }
  {
    std::lock_guard<std::mutex> lk(S().mu);
    auto it = S().cache.find(name);
    if (it != S().cache.end()) return it->second;
  }
  res = es::Resolve(*img, *spec);
  {
    std::lock_guard<std::mutex> lk(S().mu);
    S().cache.emplace(name, res);
  }
  if (res.ok) {
    Print("engine-surface: %s => %p (%s)\n", name, reinterpret_cast<void*>(res.addr), res.detail.c_str());
  } else {
    Print("engine-surface: %s UNRESOLVED: %s\n", name, res.detail.c_str());
  }
  return res;
}

void* EngineFunction(const char* name) {
  const es::Resolution r = EngineFunctionResolution(name);
  return r.ok ? reinterpret_cast<void*>(r.addr) : nullptr;
}

std::optional<int> EngineVtableIndex(const char* name) {
  const es::EngineSurface* s = GetEngineSurface();
  if (!s || !name) return std::nullopt;
  return s->VtableIndex(name);
}

namespace {

VtableVerdict VerifySlotUncached(const es::EngineSurface& s, const es::VtableSpec& spec) {
  VtableVerdict v;
  v.name = spec.name;
  v.cls = spec.cls;
  v.index = spec.index;
  if (spec.module != "server") {
    v.detail = "module " + spec.module + " is not verifiable by vtable (runtime RTTI check at use)";
    return v;
  }
  const es::Image* img = RealServerImage();
  if (!img) {
    v.detail = "real libserver.so not loaded";
    return v;
  }
  uintptr_t expected = 0;
  if (!spec.function.empty()) {
    const es::Resolution r = EngineFunctionResolution(spec.function.c_str());
    if (r.ok) expected = r.addr;
  }
  const es::VtableCheck c = es::VerifyVtable(*img, s, spec, expected);
  v.checked = true;
  v.ok = c.ok;
  v.vtable = c.vtable;
  v.target = c.target;
  v.detail = c.detail;
  return v;
}

// Cached object-less verdict for `name` (verifies on first use).
VtableVerdict SlotVerdict(const char* name) {
  VtableVerdict none;
  none.name = name ? name : "";
  const es::EngineSurface* s = GetEngineSurface();
  if (!s || !name) {
    none.detail = "engine surface unavailable";
    return none;
  }
  const es::VtableSpec* spec = s->FindVtable(name);
  if (!spec) {
    none.detail = "not listed in engine-surface.json";
    return none;
  }
  {
    std::lock_guard<std::mutex> lk(S().mu);
    auto it = S().vtables.find(name);
    if (it != S().vtables.end() && it->second.checked) return it->second;
  }
  VtableVerdict v = VerifySlotUncached(*s, *spec);  // takes S().mu internally; not held here
  if (!v.checked) return v;                         // retry later (image not loaded yet)
  {
    std::lock_guard<std::mutex> lk(S().mu);
    auto [it, inserted] = S().vtables.emplace(name, v);
    if (!inserted) return it->second;  // another thread won; keep its (identical) verdict
  }
  if (v.ok) {
    Print("engine-surface: vtable %s verified (%s)\n", name, v.detail.c_str());
  } else {
    Print("engine-surface: vtable %s UNVERIFIED: %s -- slot will not be patched or called\n", name, v.detail.c_str());
  }
  return v;
}

}  // namespace

bool VerifyEngineVtableSlot(const char* name, const void* obj, int* indexOut, std::string* whyOut) {
  const VtableVerdict v = SlotVerdict(name);
  if (!v.checked || !v.ok) {
    if (whyOut) *whyOut = v.detail.empty() ? "unverified" : v.detail;
    return false;
  }
  if (obj) {
    uintptr_t vptr = 0;
    if (!SafeRead(reinterpret_cast<uintptr_t>(obj), &vptr, sizeof(vptr))) {
      if (whyOut) *whyOut = "object unreadable";
      return false;
    }
    if (vptr != v.vtable) {
      if (whyOut) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "object vptr %p is not the verified %s vtable %p", reinterpret_cast<void*>(vptr),
                      v.cls.c_str(), reinterpret_cast<void*>(v.vtable));
        *whyOut = buf;
      }
      return false;
    }
  }
  if (indexOut) *indexOut = v.index;
  return true;
}

void VerifyAllEngineVtables() {
  const es::EngineSurface* s = GetEngineSurface();
  if (!s) return;
  for (const auto& spec : s->vtables) {
    if (spec.module == "server") (void)SlotVerdict(spec.name.c_str());
  }
}

void MarkEngineVtablePatched(const char* name, void* original) {
  if (!name) return;
  std::lock_guard<std::mutex> lk(S().mu);
  auto it = S().vtables.find(name);
  if (it == S().vtables.end()) return;
  it->second.patched = true;
  // The slot now holds our hook; keep reporting the verified original target.
  if (original) it->second.target = reinterpret_cast<uintptr_t>(original);
}

VtableVerdict EngineVtableVerdict(const char* name) {
  if (!name) return {};
  {
    std::lock_guard<std::mutex> lk(S().mu);
    auto it = S().vtables.find(name);
    if (it != S().vtables.end()) return it->second;
  }
  return SlotVerdict(name);
}

std::vector<VtableVerdict> EngineVtableReport() {
  std::vector<VtableVerdict> out;
  const es::EngineSurface* s = GetEngineSurface();
  if (!s) return out;
  for (const auto& spec : s->vtables) {
    VtableVerdict v = EngineVtableVerdict(spec.name.c_str());
    if (v.name.empty()) v.name = spec.name;
    v.cls = spec.cls;
    v.index = spec.index;
    out.push_back(std::move(v));
  }
  return out;
}

static bool EntryVerified(const std::string& name) {
  const es::EngineSurface* s = GetEngineSurface();
  if (!s) return false;
  if (s->FindVtable(name)) {
    const VtableVerdict v = EngineVtableVerdict(name.c_str());
    return v.checked && v.ok;
  }
  if (s->Find(name)) return EngineFunctionResolution(name.c_str()).ok;
  return false;
}

std::optional<int> VerifiedLayoutField(const char* layout, const char* field) {
  const es::EngineSurface* s = GetEngineSurface();
  if (!s || !layout || !field) return std::nullopt;
  const es::LayoutSpec* l = s->FindLayout(layout);
  if (!l || !EntryVerified(l->verified_by)) return std::nullopt;
  return l->Field(field);
}

std::vector<LayoutVerdict> EngineLayoutReport() {
  std::vector<LayoutVerdict> out;
  const es::EngineSurface* s = GetEngineSurface();
  if (!s) return out;
  for (const auto& l : s->layouts) {
    LayoutVerdict v;
    v.name = l.name;
    v.verified_by = l.verified_by;
    v.fields = l.fields;
    v.ok = EntryVerified(l.verified_by);
    out.push_back(std::move(v));
  }
  return out;
}

bool SafeReadMemory(uintptr_t addr, void* out, size_t n) { return addr && SafeRead(addr, out, n); }

std::optional<std::string> SafeReadCString(uintptr_t addr, size_t maxLen) {
  if (!addr || maxLen == 0) return std::nullopt;
  std::string out;
  char buf[256];
  while (out.size() < maxLen) {
    // Never cross a page boundary in one read: the next page may be unmapped.
    const uintptr_t cur = addr + out.size();
    const size_t toPageEnd = 4096 - (cur & 4095);
    size_t n = std::min({sizeof(buf), toPageEnd, maxLen - out.size()});
    if (!SafeRead(cur, buf, n)) return std::nullopt;
    const void* nul = std::memchr(buf, 0, n);
    if (nul) {
      out.append(buf, static_cast<const char*>(nul) - buf);
      return out;
    }
    out.append(buf, n);
  }
  return std::nullopt;  // no terminator within maxLen
}

std::optional<std::string> ObjectRttiName(const void* obj) {
  if (!obj) return std::nullopt;
  uintptr_t vptr = 0, typeinfo = 0, name = 0;
  if (!SafeRead(reinterpret_cast<uintptr_t>(obj), &vptr, sizeof(vptr)) || !vptr) return std::nullopt;
  if (!SafeRead(vptr - sizeof(void*), &typeinfo, sizeof(typeinfo)) || !typeinfo) return std::nullopt;
  if (!SafeRead(typeinfo + sizeof(void*), &name, sizeof(name)) || !name) return std::nullopt;
  return SafeReadCString(name, 256);
}

bool ObjectHasRttiAnyModule(const void* obj, const char* cls) {
  const es::EngineSurface* s = GetEngineSurface();
  if (!s || !obj || !cls) return false;
  const auto want = s->RttiName(cls);
  if (!want) return false;
  const auto got = ObjectRttiName(obj);
  return got && *got == *want;
}

bool ObjectHasEngineRtti(const void* obj, const char* cls) {
  const es::EngineSurface* s = GetEngineSurface();
  const es::Image* img = RealServerImage();
  if (!s || !img || !obj || !cls) return false;
  const auto want = s->RttiName(cls);
  if (!want) return false;

  // Reads inside the (known-mapped) server image go straight to memory; anything else
  // (e.g. a heap object) goes through process_vm_readv so a bad pointer can't fault.
  auto read = [img](uintptr_t addr, uintptr_t* out) -> bool {
    if (const uint8_t* b = img->Bytes(addr, sizeof(*out))) {
      std::memcpy(out, b, sizeof(*out));
      return true;
    }
    return SafeRead(addr, out, sizeof(*out));
  };
  uintptr_t vptr = 0, typeinfo = 0, name = 0;
  if (!read(reinterpret_cast<uintptr_t>(obj), &vptr)) return false;
  if (!img->Contains(vptr) || !img->Contains(vptr - sizeof(void*))) return false;
  if (!read(vptr - sizeof(void*), &typeinfo)) return false;
  if (!img->Contains(typeinfo)) return false;
  if (!read(typeinfo + sizeof(void*), &name)) return false;
  const auto got = img->CString(name, 128);
  return got && *got == *want;
}

}  // namespace readyup
