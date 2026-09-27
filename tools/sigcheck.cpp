// Offline engine-surface checker.
//
//   readyup_sigcheck <path/to/valve/libserver.so> [gamedata/engine-surface.json]
//
// Maps the PT_LOAD segments of Valve's libserver.so from disk (no execution) and runs the
// exact same signature + anchor verification the plugin runs at load time, plus the RTTI-based
// vtable slot checks and struct-layout anchors. Exit code 0 only if every function resolves
// uniquely and passes its anchors, every libserver RTTI name exists and has one primary vtable, every
// vtable slot verifies and every layout is pinned by verified code.
//
// Note: .data.rel.ro holds R_X86_64_RELATIVE targets as in-place addends in the file, so the
// vtable/typeinfo pointers read from disk are RVAs, exactly like the runtime (relocated) values.

#include "readyup/engine_surface_core.h"

#include <elf.h>

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace readyup::es;

static std::string ReadFile(const char* path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}


// Parses argv[2] (default gamedata/engine-surface.json) and merges every further argument
// (fragments such as gamedata/engine-surface.skins.json) into it.
static std::optional<EngineSurface> LoadSurfaces(int argc, char** argv) {
  const char* basePath = argc > 2 ? argv[2] : "gamedata/engine-surface.json";
  std::string err;
  auto es = ParseEngineSurface(ReadFile(basePath), &err);
  if (!es) {
    std::fprintf(stderr, "%s: parse error: %s\n", basePath, err.c_str());
    return std::nullopt;
  }
  for (int i = 3; i < argc; ++i) {
    auto frag = ParseEngineSurface(ReadFile(argv[i]), &err);
    if (!frag || !MergeEngineSurface(&*es, *frag, &err)) {
      std::fprintf(stderr, "%s: %s\n", argv[i], err.c_str());
      return std::nullopt;
    }
    std::printf("merged fragment %s (%zu functions, %zu vtables)\n", argv[i], frag->functions.size(),
                frag->vtables.size());
  }
  return es;
}

// A module binary on disk: its bytes and the PT_LOAD segments at their ELF vaddrs.
struct LoadedModule {
  std::string bin;
  Image img;
};

static bool LoadModule(const char* path, LoadedModule* out) {
  out->bin = ReadFile(path);
  if (out->bin.size() < sizeof(Elf64_Ehdr)) return false;
  const auto* eh = reinterpret_cast<const Elf64_Ehdr*>(out->bin.data());
  for (int i = 0; i < eh->e_phnum; ++i) {
    const auto* ph = reinterpret_cast<const Elf64_Phdr*>(out->bin.data() + eh->e_phoff + i * eh->e_phentsize);
    if (ph->p_type != PT_LOAD) continue;
    Region r;
    r.addr = ph->p_vaddr;
    r.data = reinterpret_cast<const uint8_t*>(out->bin.data()) + ph->p_offset;
    r.size = ph->p_filesz;
    r.exec = (ph->p_flags & PF_X) != 0;
    out->img.regions.push_back(r);
  }
  return true;
}

int main(int argc, char** argv) {
  // --lib <module>=<path> (repeatable): entries whose library / module is <module> are checked
  // against that binary ("engine2" = libengine2.so). Entries of a module with no binary are SKIPped.
  std::map<std::string, LoadedModule> modules;
  std::vector<char*> args{argv[0]};
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--lib" && i + 1 < argc) {
      const std::string spec = argv[++i];
      const auto eq = spec.find('=');
      if (eq == std::string::npos || eq == 0 || !LoadModule(spec.c_str() + eq + 1, &modules[spec.substr(0, eq)])) {
        std::fprintf(stderr, "bad --lib %s (want <module>=<path to lib<module>.so>)\n", spec.c_str());
        return 2;
      }
      continue;
    }
    args.push_back(argv[i]);
  }
  if (args.size() < 2) {
    std::fprintf(stderr,
                 "usage: %s <libserver.so> [engine-surface.json [fragment.json ...]] [--lib <module>=<lib.so> ...]\n",
                 argv[0]);
    return 2;
  }
  if (!LoadModule(args[1], &modules["server"])) {
    std::fprintf(stderr, "failed to read inputs\n");
    return 2;
  }
  auto moduleOf = [&](const std::string& m) -> const LoadedModule* {
    auto it = modules.find(m);
    return it == modules.end() ? nullptr : &it->second;
  };

  auto es = LoadSurfaces(static_cast<int>(args.size()), args.data());
  if (!es) return 2;
  std::printf("engine-surface for game %s\n", es->game_version.c_str());

  int bad = 0;
  std::map<std::string, uintptr_t> resolved;  // verified functions only
  for (const auto& f : es->functions) {
    const LoadedModule* m = moduleOf(f.library);
    if (!m) {
      std::printf("SKIP %-45s (lib%s.so not given: --lib %s=...)\n", f.name.c_str(), f.library.c_str(), f.library.c_str());
      continue;
    }
    const Resolution r = Resolve(m->img, f);
    std::printf("%-4s %-45s %s%s matches=%d rva=0x%lx  %s\n", r.ok ? "OK" : "FAIL", f.name.c_str(),
                f.required ? "required" : "optional", f.library == "server" ? "" : (" [" + f.library + "]").c_str(),
                r.matches, static_cast<unsigned long>(r.addr), r.detail.c_str());
    if (r.ok) resolved[f.name] = r.addr;
    else ++bad;
  }

  // RTTI classes. The plugin checks object vtables against these typeinfo names before virtual
  // calls, so a renamed/removed class would silently disable a feature:
  //  1) the mangled class name must still exist as a NUL-terminated string in the image, and
  //  2) it must lead to exactly one primary vtable (used to verify vtable_indices slots).
  for (const auto& rt : es->rtti) {
    const LoadedModule* m = moduleOf(rt.module);
    if (!m) {
      std::printf("SKIP rtti   %-38s %s (module %s: runtime check only)\n", rt.cls.c_str(), rt.typeinfo_name.c_str(),
                  rt.module.c_str());
      continue;
    }
    const std::string needle = rt.typeinfo_name + std::string(1, '\0');
    if (m->bin.find(needle) == std::string::npos) {
      std::printf("FAIL rtti   %-38s %s  class name not in binary\n", rt.cls.c_str(), rt.typeinfo_name.c_str());
      ++bad;
      continue;
    }
    std::string detail;
    const auto vt = FindVtableByRtti(m->img, rt.typeinfo_name, &detail);
    std::printf("%-4s rtti   %-38s %s  name ok; %s\n", vt ? "OK" : "FAIL", rt.cls.c_str(), rt.typeinfo_name.c_str(),
                detail.c_str());
    if (!vt) ++bad;
  }

  // Virtual slots: vtable located via RTTI, slot target checked against function/anchors.
  std::map<std::string, bool> verified;  // vtables + functions, for layouts
  for (const auto& kv : resolved) verified[kv.first] = true;
  for (const auto& v : es->vtables) {
    const LoadedModule* m = moduleOf(v.module);
    if (!m) {
      std::printf("SKIP vtable %-38s [%d] (module %s: runtime check only)\n", v.name.c_str(), v.index, v.module.c_str());
      continue;
    }
    uintptr_t expected = 0;
    if (!v.function.empty()) {
      auto it = resolved.find(v.function);
      if (it != resolved.end()) expected = it->second;
    }
    const VtableCheck c = VerifyVtable(m->img, *es, v, expected);
    std::printf("%-4s vtable %-38s [%d] %s\n", c.ok ? "OK" : "FAIL", v.name.c_str(), v.index, c.detail.c_str());
    verified[v.name] = c.ok;
    if (!c.ok) ++bad;
  }

  // Struct layouts: trusted only if the code that reads them verified (mov_disp anchors).
  for (const auto& l : es->layouts) {
    const auto it = verified.find(l.verified_by);
    const bool ok = it != verified.end() && it->second;
    std::string fields;
    for (const auto& f : l.fields) fields += " " + f.first + "=0x" + [&] {
      char b[16];
      std::snprintf(b, sizeof(b), "%x", f.second);
      return std::string(b);
    }();
    std::printf("%-4s layout %-38s%s (verified by %s)\n", ok ? "OK" : "FAIL", l.name.c_str(), fields.c_str(),
                l.verified_by.c_str());
    if (!ok) ++bad;
  }
  std::printf("sigcheck: %s (%d failure%s)\n", bad == 0 ? "PASS" : "FAIL", bad, bad == 1 ? "" : "s");
  return bad == 0 ? 0 : 1;
}
