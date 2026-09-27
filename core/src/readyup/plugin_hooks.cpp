#include "readyup/plugin_hooks.h"

#include "readyup/engine_surface.h"
#include "readyup/logging.h"

#include <sys/mman.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "third_party/funchook/include/funchook.h"

namespace readyup::plugins::hooks {
namespace {

struct FnHook {
  int owner = 0;
  std::string key;
  funchook_t* fh = nullptr;
};

struct VtHook {
  int owner = 0;
  std::string key;
  void** slot = nullptr;
  void* original = nullptr;
  void* replacement = nullptr;
};

std::mutex g_mu;
std::vector<FnHook> g_fn;
std::vector<VtHook> g_vt;

bool WriteSlot(void** slot, void* value) {
  const uintptr_t page = reinterpret_cast<uintptr_t>(slot) & ~(static_cast<uintptr_t>(getpagesize() - 1));
  if (mprotect(reinterpret_cast<void*>(page), static_cast<size_t>(getpagesize()), PROT_READ | PROT_WRITE) != 0) {
    return false;
  }
  *slot = value;
  mprotect(reinterpret_cast<void*>(page), static_cast<size_t>(getpagesize()), PROT_READ);
  return true;
}

}  // namespace

void* SurfaceFunction(const char* key) { return key ? EngineFunction(key) : nullptr; }

bool HookFunction(int owner, const std::string& plugin, const char* key, void* detour, void** trampoline,
                  std::string* why) {
  auto fail = [&](const std::string& w) {
    if (why) *why = w;
    Print("plugin-hooks: %s: hook %s refused: %s\n", plugin.c_str(), key ? key : "?", w.c_str());
    return false;
  };
  if (!key || !detour || !trampoline) return fail("bad arguments");
  const es::EngineSurface* s = GetEngineSurface();
  const es::FunctionSpec* spec = s ? s->Find(key) : nullptr;
  if (!spec) return fail("not listed in the engine surface (ship it in the plugin's engine-surface.<name>.json)");
  // Only entries marked for detouring: readyup_hookcheck checks their prologue on every CS2 build.
  if (spec->hook != "funchook") return fail("entry is not marked \"hook\": \"funchook\"");
  void* target = EngineFunction(key);
  if (!target) return fail("unresolved on this build");
  std::lock_guard<std::mutex> lk(g_mu);
  for (const auto& h : g_fn) {
    if (h.key == key) return fail("already hooked by another plugin");
  }
  funchook_t* fh = funchook_create();
  if (!fh) return fail("funchook_create failed");
  void* fn = target;
  int rv = funchook_prepare(fh, &fn, detour);
  if (rv == 0) rv = funchook_install(fh, 0);
  if (rv != 0) {
    const std::string msg = funchook_error_message(fh);
    funchook_destroy(fh);
    return fail("funchook: " + msg);
  }
  *trampoline = fn;
  g_fn.push_back({owner, key, fh});
  Print("plugin-hooks: %s hooked %s\n", plugin.c_str(), key);
  return true;
}

bool HookVtable(int owner, const std::string& plugin, const char* key, const void* obj, void* fn, void** original,
                std::string* why) {
  auto fail = [&](const std::string& w) {
    if (why) *why = w;
    Print("plugin-hooks: %s: vtable hook %s refused: %s\n", plugin.c_str(), key ? key : "?", w.c_str());
    return false;
  };
  if (!key || !obj || !fn || !original) return fail("bad arguments");
  int index = -1;
  std::string vwhy;
  // Slot target and the object's vptr against the RTTI-located vtable, as for the core's own slots.
  if (!VerifyEngineVtableSlot(key, obj, &index, &vwhy)) return fail(vwhy.empty() ? "slot did not verify" : vwhy);
  void** vtable = *reinterpret_cast<void** const*>(obj);
  void** slot = &vtable[index];
  std::lock_guard<std::mutex> lk(g_mu);
  for (const auto& h : g_vt) {
    if (h.slot == slot) return fail("slot already hooked by another plugin");
  }
  void* orig = *slot;
  if (!WriteSlot(slot, fn)) return fail("mprotect failed");
  *original = orig;
  g_vt.push_back({owner, key, slot, orig, fn});
  MarkEngineVtablePatched(key, orig);
  Print("plugin-hooks: %s hooked vtable %s [%d]\n", plugin.c_str(), key, index);
  return true;
}

void DropPluginHooks(int owner, const std::string& plugin) {
  int n = 0;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto it = g_vt.begin(); it != g_vt.end();) {
      if (it->owner != owner) {
        ++it;
        continue;
      }
      if (*it->slot == it->replacement) WriteSlot(it->slot, it->original);  // untouched by others since
      it = g_vt.erase(it);
      ++n;
    }
    for (auto it = g_fn.begin(); it != g_fn.end();) {
      if (it->owner != owner) {
        ++it;
        continue;
      }
      funchook_uninstall(it->fh, 0);
      funchook_destroy(it->fh);
      it = g_fn.erase(it);
      ++n;
    }
  }
  if (n) {
    Print("plugin-hooks: removed %d hook(s) of %s\n", n, plugin.c_str());
    // Another thread may still be inside a detour of the image that is about to be closed.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

std::vector<std::string> Report() {
  std::lock_guard<std::mutex> lk(g_mu);
  std::vector<std::string> out;
  for (const auto& h : g_fn) out.push_back("fn " + h.key + " (plugin " + std::to_string(h.owner) + ")");
  for (const auto& h : g_vt) out.push_back("vtable " + h.key + " (plugin " + std::to_string(h.owner) + ")");
  return out;
}

}  // namespace readyup::plugins::hooks
