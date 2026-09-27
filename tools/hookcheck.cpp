// Offline hook-site safety check.
//
//   readyup_hookcheck <path/to/valve/libserver.so> [gamedata/engine-surface.json]
//
// For every function in engine-surface.json marked "hook": "funchook", this resolves the
// function exactly like the plugin does (signature + anchors), then runs funchook_prepare()
// on it against a copy of Valve's image mapped at its ELF layout. funchook_prepare decodes the
// prologue that the detour jump would overwrite, relocates it into a trampoline (fixing up
// RIP-relative operands and branches) and fails if that is not possible, e.g. a branch back
// into the overwritten bytes or an instruction it cannot relocate. It never writes to the
// target (that is funchook_install, which we do not call), so nothing is executed.
//
// Exit code 0 only if every hook site resolves and its prologue relocates.

#include "readyup/engine_surface_core.h"
#include "readyup/minijson.h"

#include <elf.h>
#include <sys/mman.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <map>
#include <vector>

#include "funchook.h"

using namespace readyup;

static std::string ReadFile(const char* path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// The detour's hook_func is never called; it only has to be a real code address.
extern "C" void readyup_hookcheck_dummy_detour() {}


// Parses argv[2] (default gamedata/engine-surface.json) and merges every further argument
// (fragments such as gamedata/engine-surface.skins.json) into it.
static std::optional<es::EngineSurface> LoadSurfaces(int argc, char** argv) {
  const char* basePath = argc > 2 ? argv[2] : "gamedata/engine-surface.json";
  std::string err;
  auto es = es::ParseEngineSurface(ReadFile(basePath), &err);
  if (!es) {
    std::fprintf(stderr, "%s: parse error: %s\n", basePath, err.c_str());
    return std::nullopt;
  }
  for (int i = 3; i < argc; ++i) {
    auto frag = es::ParseEngineSurface(ReadFile(argv[i]), &err);
    if (!frag || !es::MergeEngineSurface(&*es, *frag, &err)) {
      std::fprintf(stderr, "%s: %s\n", argv[i], err.c_str());
      return std::nullopt;
    }
    std::printf("merged fragment %s (%zu functions, %zu vtables)\n", argv[i], frag->functions.size(),
                frag->vtables.size());
  }
  return es;
}

// One module mapped for funchook: every PT_LOAD at base + p_vaddr in one contiguous reservation
// so RIP-relative targets keep their real distances (funchook allocates the trampoline within +-2GB).
struct MappedModule {
  uint8_t* base = nullptr;
  es::Image img;
};

static bool MapModule(const char* path, MappedModule* out) {
  const std::string bin = ReadFile(path);
  if (bin.size() < sizeof(Elf64_Ehdr) || std::memcmp(bin.data(), ELFMAG, SELFMAG) != 0) return false;
  const auto* eh = reinterpret_cast<const Elf64_Ehdr*>(bin.data());
  std::vector<const Elf64_Phdr*> loads;
  uint64_t span = 0;
  for (int i = 0; i < eh->e_phnum; ++i) {
    const auto* ph = reinterpret_cast<const Elf64_Phdr*>(bin.data() + eh->e_phoff + i * eh->e_phentsize);
    if (ph->p_type != PT_LOAD) continue;
    loads.push_back(ph);
    if (ph->p_vaddr + ph->p_memsz > span) span = ph->p_vaddr + ph->p_memsz;
  }
  span = (span + 0xfff) & ~uint64_t{0xfff};
  void* mem = mmap(nullptr, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) {
    std::perror("mmap");
    return false;
  }
  out->base = static_cast<uint8_t*>(mem);
  for (const auto* ph : loads) {
    std::memcpy(out->base + ph->p_vaddr, bin.data() + ph->p_offset, ph->p_filesz);
    es::Region r;
    r.addr = reinterpret_cast<uintptr_t>(out->base) + ph->p_vaddr;
    r.data = out->base + ph->p_vaddr;
    r.size = ph->p_filesz;
    r.exec = (ph->p_flags & PF_X) != 0;
    out->img.regions.push_back(r);
  }
  return true;
}

int main(int argc, char** argv) {
  // --lib <module>=<path> (repeatable): hooked entries of that module ("library") are checked
  // against it; entries of a module with no binary are SKIPped.
  std::map<std::string, MappedModule> mods;
  std::vector<char*> args{argv[0]};
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--lib" && i + 1 < argc) {
      const std::string spec = argv[++i];
      const auto eq = spec.find('=');
      if (eq == std::string::npos || eq == 0 || !MapModule(spec.c_str() + eq + 1, &mods[spec.substr(0, eq)])) {
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
  if (!MapModule(args[1], &mods["server"])) {
    std::fprintf(stderr, "failed to read inputs\n");
    return 2;
  }

  auto surface = LoadSurfaces(static_cast<int>(args.size()), args.data());
  if (!surface) return 2;

  int checked = 0, bad = 0;
  for (const auto& f : surface->functions) {
    if (f.hook != "funchook") continue;
    auto mit = mods.find(f.library);
    if (mit == mods.end()) {
      std::printf("SKIP %-40s (lib%s.so not given: --lib %s=...)\n", f.name.c_str(), f.library.c_str(), f.library.c_str());
      continue;
    }
    ++checked;
    uint8_t* base = mit->second.base;

    const es::Resolution r = es::Resolve(mit->second.img, f);
    if (!r.ok) {
      std::printf("FAIL %-40s unresolved (matches=%d) %s\n", f.name.c_str(), r.matches, r.detail.c_str());
      ++bad;
      continue;
    }
    const uintptr_t rva = r.addr - reinterpret_cast<uintptr_t>(base);

    funchook_t* fh = funchook_create();
    if (!fh) {
      std::printf("FAIL %-40s funchook_create failed\n", f.name.c_str());
      ++bad;
      continue;
    }
    void* target = reinterpret_cast<void*>(r.addr);
    const int rv = funchook_prepare(fh, &target, reinterpret_cast<void*>(&readyup_hookcheck_dummy_detour));
    char prologue[3 * 16 + 1] = {};
    for (int i = 0; i < 16; ++i) std::snprintf(prologue + 3 * i, 4, "%02X ", base[rva + i]);
    if (rv != 0) {
      std::printf("FAIL %-40s rva=0x%lx funchook_prepare=%d: %s  prologue: %s\n", f.name.c_str(),
                  static_cast<unsigned long>(rva), rv, funchook_error_message(fh), prologue);
      ++bad;
    } else {
      std::printf("OK   %-40s rva=0x%lx prologue relocates  prologue: %s\n", f.name.c_str(),
                  static_cast<unsigned long>(rva), prologue);
    }
    funchook_destroy(fh);
  }

  if (checked == 0) {
    std::printf("no functions marked \"hook\": \"funchook\" in engine-surface.json\n");
    return 1;
  }
  std::printf("%d hook site(s) checked, %d failed\n", checked, bad);
  return bad == 0 ? 0 : 1;
}
