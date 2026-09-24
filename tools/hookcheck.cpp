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

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <libserver.so> [engine-surface.json]\n", argv[0]);
    return 2;
  }
  const std::string bin = ReadFile(argv[1]);
  const std::string json = ReadFile(argc > 2 ? argv[2] : "gamedata/engine-surface.json");
  if (bin.size() < sizeof(Elf64_Ehdr) || json.empty() || std::memcmp(bin.data(), ELFMAG, SELFMAG) != 0) {
    std::fprintf(stderr, "failed to read inputs\n");
    return 2;
  }

  // Map every PT_LOAD at base + p_vaddr in one contiguous reservation so RIP-relative
  // targets keep their real distances (funchook allocates the trampoline within +-2GB).
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
    return 2;
  }
  auto* base = static_cast<uint8_t*>(mem);
  es::Image img;
  for (const auto* ph : loads) {
    std::memcpy(base + ph->p_vaddr, bin.data() + ph->p_offset, ph->p_filesz);
    es::Region r;
    r.addr = reinterpret_cast<uintptr_t>(base) + ph->p_vaddr;
    r.data = base + ph->p_vaddr;
    r.size = ph->p_filesz;
    r.exec = (ph->p_flags & PF_X) != 0;
    img.regions.push_back(r);
  }

  std::string err;
  auto surface = es::ParseEngineSurface(json, &err);
  if (!surface) {
    std::fprintf(stderr, "engine-surface parse error: %s\n", err.c_str());
    return 2;
  }
  minijson::ParseError perr;
  auto root = minijson::Parse(json, &perr);
  const minijson::Value* fns = root ? root->get("functions") : nullptr;
  if (!minijson::IsObject(fns)) {
    std::fprintf(stderr, "engine-surface: no functions object\n");
    return 2;
  }

  int checked = 0, bad = 0;
  for (const auto& f : surface->functions) {
    const minijson::Value* raw = fns->get(f.name.c_str());
    auto hook = raw ? minijson::AsString(raw->get("hook")) : std::nullopt;
    if (!hook || *hook != "funchook") continue;
    ++checked;

    const es::Resolution r = es::Resolve(img, f);
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
