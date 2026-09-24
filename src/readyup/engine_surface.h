#pragma once

// Runtime access to the repo-owned engine surface (gamedata/engine-surface.json).
//
// Source order: <shim dir>/engine-surface.json if present (lets an operator hotfix a
// signature without rebuilding), otherwise the copy embedded into the shim at build time.
// The CounterStrikeSharp CDN gamedata is not consulted.

#include "readyup/engine_surface_core.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace readyup {

const es::EngineSurface* GetEngineSurface();

// Verified address of an engine function: exactly one signature match in the real
// libserver.so AND all identity anchors pass. nullptr otherwise (never a guess).
// Results are cached per name; failures are logged once.
void* EngineFunction(const char* name);

// Full resolution report (uncached re-run is not needed; returns the cached result).
es::Resolution EngineFunctionResolution(const char* name);

// Raw slot index from engine-surface.json. Prefer VerifyEngineVtableSlot(), which only hands out
// an index once the slot verified.
std::optional<int> EngineVtableIndex(const char* name);

// ---- Virtual slots (vtable_indices) ------------------------------------------------------
//
// Verification (cached, logged once): the implementing class's primary vtable is located in the
// real libserver.so through its RTTI typeinfo name, and the slot's target must equal the entry's
// `function` and/or pass its anchors -- the same check readyup_sigcheck runs offline.
// With `obj`, the object's vptr must also equal that vtable (a mismatch rejects only this object).
// Returns true and sets *indexOut only if everything passed; *whyOut explains a failure.
bool VerifyEngineVtableSlot(const char* name, const void* obj, int* indexOut, std::string* whyOut);

// Runs the (object-less) verification for every libserver vtable entry. Call once at load.
void VerifyAllEngineVtables();

// Records that a verified slot was patched (for `ru selftest`).
void MarkEngineVtablePatched(const char* name, void* original);

struct VtableVerdict {
  std::string name;
  std::string cls;
  int index = -1;
  bool checked = false;  // false: not verified yet (image not loaded / not attempted)
  bool ok = false;
  bool patched = false;
  uintptr_t vtable = 0;
  uintptr_t target = 0;
  std::string detail;
};
std::vector<VtableVerdict> EngineVtableReport();
// Verdict for one entry (checked=false if unknown / not yet verified).
VtableVerdict EngineVtableVerdict(const char* name);

// ---- Struct layouts (layouts) ------------------------------------------------------------
// A field offset, only if the layout's `verified_by` vtable slot or function verified.
std::optional<int> VerifiedLayoutField(const char* layout, const char* field);

struct LayoutVerdict {
  std::string name;
  std::string verified_by;
  bool ok = false;
  std::vector<std::pair<std::string, int>> fields;
};
std::vector<LayoutVerdict> EngineLayoutReport();

// ---- RTTI --------------------------------------------------------------------------------
// True if `obj` is readable, its vptr points into the real libserver.so, and the vtable's
// RTTI typeinfo name equals the engine-surface "rtti" entry for `cls`.
bool ObjectHasEngineRtti(const void* obj, const char* cls);

// Like ObjectHasEngineRtti for classes living in any module (e.g. libschemasystem.so): every
// read goes through process_vm_readv, so a bad pointer cannot fault.
bool ObjectHasRttiAnyModule(const void* obj, const char* cls);

// Mangled RTTI typeinfo name of a polymorphic object (fault-safe reads), or nullopt.
std::optional<std::string> ObjectRttiName(const void* obj);

// Fault-safe reads (process_vm_readv): false / nullopt instead of SIGSEGV on a bad pointer.
bool SafeReadMemory(uintptr_t addr, void* out, size_t n);
std::optional<std::string> SafeReadCString(uintptr_t addr, size_t maxLen);

// Snapshot of the real Valve server module's loaded segments (runtime addresses).
const es::Image* RealServerImage();

}  // namespace readyup
