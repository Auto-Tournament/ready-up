#include "readyup/schema.h"

#include "readyup/config.h"
#include "readyup/engine_surface.h"
#include "readyup/logging.h"

#include <dlfcn.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>

namespace readyup {
namespace {

// SchemaSystem access via its virtual interface (verified against CS2 1.41.8.3 / buildid 25492732
// with an offline harness that loads libschemasystem.so + libserver.so and calls
// InstallSchemaBindings):
//
//   ISchemaSystem vtbl[13]          FindTypeScopeForModule(const char* module, void* = nullptr)
//   CSchemaSystemTypeScope vtbl[2]  FindDeclaredClass(const char* name) -> SchemaClassInfoData_t*
//
// Neither slot has a string anchor in libschemasystem.so, so they are guarded by RTTI instead:
// the object must be a CSchemaSystem / CSchemaSystemTypeScope (engine-surface "rtti") before the
// slot is called, and the returned class info must round-trip (DetectLayout).
//
// SchemaClassInfoData_t gained an extra pointer at +0x18 in 2025/2026 builds. We detect the
// layout at runtime (see DetectLayout) and refuse to guess when neither known layout verifies.

constexpr int kVtFindTypeScopeForModule = 13;
constexpr int kVtFindDeclaredClass = 2;

struct FieldData {
  const char* name;
  void* type;
  int32_t offset;
  int32_t metaCount;
  void* meta;
};
static_assert(sizeof(FieldData) == 0x20, "SchemaClassFieldData_t is 0x20 bytes");

struct BaseClassData {
  uint32_t offset;
  uint32_t pad;
  const void* cls;  // SchemaClassInfoData_t*
};
static_assert(sizeof(BaseClassData) == 0x10, "SchemaBaseClassInfoData_t is 0x10 bytes");

// Offsets inside SchemaClassInfoData_t.
struct ClassLayout {
  int name = 0x08;
  int size = 0x20;
  int fieldCount = 0x24;  // uint16
  int baseCount = 0x29;   // uint8
  int fields = 0x30;      // FieldData*
  int bases = 0x38;       // BaseClassData*
};

// Layout used by builds before the +0x18 insertion (hl2sdk-cs2 2024).
constexpr ClassLayout kLegacyLayout{0x08, 0x18, 0x1C, 0x23, 0x28, 0x38};
constexpr ClassLayout kCurrentLayout{0x08, 0x20, 0x24, 0x29, 0x30, 0x38};

std::atomic<bool> g_inited{false};
std::atomic<int> g_status{0};  // 0 pending, 1 ok, 2 failed
void* g_schema = nullptr;
void* g_serverScope = nullptr;
ClassLayout g_layout = kCurrentLayout;

std::mutex g_mu;  // guards the maps + g_detail
std::string g_detail = "libschemasystem.so not loaded yet";
std::unordered_map<std::string, int> g_cache;
std::map<std::pair<std::string, std::string>, int> g_lookups;  // (class, field) -> offset or -1

void SetDetail(std::string d) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_detail = std::move(d);
}

void Fail(const std::string& why) {
  SetDetail(why);
  g_status.store(2);
  Print("schema: %s; schema offsets disabled (features needing them turn off).\n", why.c_str());
}

template <typename T>
T ReadField(const void* base, int off) {
  T v{};
  std::memcpy(&v, reinterpret_cast<const unsigned char*>(base) + off, sizeof(T));
  return v;
}

template <typename R, typename... A>
R VCall(void* obj, int idx, A... a) {
  void** vt = *reinterpret_cast<void***>(obj);
  return reinterpret_cast<R (*)(void*, A...)>(vt[idx])(obj, a...);
}

const void* FindClass(const char* name) {
  if (!g_serverScope || !name || !*name) return nullptr;
  return VCall<const void*>(g_serverScope, kVtFindDeclaredClass, name);
}

bool LayoutLooksRight(const void* cls, const ClassLayout& l, const char* expectName, int expectSize) {
  if (!cls) return false;
  // Read the candidate header fault-safely: a wrong layout must not crash, just not verify.
  unsigned char hdr[0x40];
  if (!SafeReadMemory(reinterpret_cast<uintptr_t>(cls), hdr, sizeof(hdr))) return false;
  const char* nm = ReadField<const char*>(hdr, l.name);
  const auto nmStr = SafeReadCString(reinterpret_cast<uintptr_t>(nm), 64);
  if (!nmStr || *nmStr != expectName) return false;
  if (ReadField<int32_t>(hdr, l.size) != expectSize) return false;
  const uint16_t nf = ReadField<uint16_t>(hdr, l.fieldCount);
  if (nf == 0 || nf > 64) return false;
  const FieldData* f = ReadField<const FieldData*>(hdr, l.fields);
  FieldData f0{};
  return f && SafeReadMemory(reinterpret_cast<uintptr_t>(f), &f0, sizeof(f0)) && f0.name != nullptr;
}

// CEntityInstance is tiny and stable (vtable + m_iszPrivateVScripts + m_pEntity + ...; 0x30 bytes).
bool DetectLayout() {
  const void* c = FindClass("CEntityInstance");
  if (LayoutLooksRight(c, kCurrentLayout, "CEntityInstance", 0x30)) {
    g_layout = kCurrentLayout;
    if (DebugEnabled()) PrintLine("schema: class-info layout = current (+0x18 inserted)");
    return true;
  }
  if (LayoutLooksRight(c, kLegacyLayout, "CEntityInstance", 0x30)) {
    g_layout = kLegacyLayout;
    if (DebugEnabled()) PrintLine("schema: class-info layout = legacy");
    return true;
  }
  return false;
}

std::optional<int> FindFieldRecursive(const void* cls, const char* field, int depth) {
  if (!cls || depth > 24) return std::nullopt;
  const auto& l = g_layout;
  const uint16_t nf = ReadField<uint16_t>(cls, l.fieldCount);
  const FieldData* fields = ReadField<const FieldData*>(cls, l.fields);
  if (fields) {
    for (uint16_t i = 0; i < nf; ++i) {
      if (fields[i].name && std::strcmp(fields[i].name, field) == 0) return fields[i].offset;
    }
  }
  const uint8_t nb = ReadField<uint8_t>(cls, l.baseCount);
  const BaseClassData* bases = ReadField<const BaseClassData*>(cls, l.bases);
  if (bases) {
    for (uint8_t b = 0; b < nb; ++b) {
      if (auto off = FindFieldRecursive(bases[b].cls, field, depth + 1)) {
        return *off + static_cast<int>(bases[b].offset);
      }
    }
  }
  return std::nullopt;
}

void Record(const std::string& cls, const std::string& field, std::optional<int> off) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_lookups[{cls, field}] = off ? *off : -1;
}

}  // namespace

bool SchemaInit() {
  const int st = g_status.load();
  if (st == 1) return true;
  if (st == 2) return false;
  static std::mutex s_initMu;
  std::lock_guard<std::mutex> lk(s_initMu);
  if (g_status.load() != 0) return g_status.load() == 1;
  // Retry on every call until it works (libserver may not have installed its bindings yet), but
  // only log the first failure.
  const bool firstAttempt = !g_inited.exchange(true);

  void* schemasys = dlopen("libschemasystem.so", RTLD_NOW | RTLD_NOLOAD);
  if (!schemasys) {
    SetDetail("libschemasystem.so not loaded yet");
    if (firstAttempt && DebugEnabled()) PrintLine("schema: libschemasystem.so not loaded");
    return false;
  }

  using CreateInterfaceFn = void* (*)(const char* name, int* returnCode);
  auto createInterface = reinterpret_cast<CreateInterfaceFn>(dlsym(schemasys, "CreateInterface"));
  if (!createInterface) {
    Fail("CreateInterface not exported by libschemasystem.so");
    return false;
  }

  if (!g_schema) {
    void* schema = createInterface("SchemaSystem_001", nullptr);
    if (!schema) {
      Fail("SchemaSystem_001 interface not available");
      return false;
    }
    if (!ObjectHasRttiAnyModule(schema, "CSchemaSystem")) {
      const auto got = ObjectRttiName(schema);
      Fail("SchemaSystem_001 is not a CSchemaSystem (RTTI " + (got ? *got : std::string("unreadable")) + ")");
      return false;
    }
    g_schema = schema;
  }

  void* scope = VCall<void*>(g_schema, kVtFindTypeScopeForModule, "libserver.so", static_cast<void*>(nullptr));
  if (!scope) {
    SetDetail("FindTypeScopeForModule(libserver.so) returned null (bindings not installed yet)");
    if (firstAttempt) PrintLine("schema: FindTypeScopeForModule(libserver.so) returned null; will retry.");
    return false;
  }
  if (!ObjectHasRttiAnyModule(scope, "CSchemaSystemTypeScope")) {
    const auto got = ObjectRttiName(scope);
    Fail("libserver.so type scope is not a CSchemaSystemTypeScope (RTTI " + (got ? *got : std::string("unreadable")) + ")");
    return false;
  }
  g_serverScope = scope;

  if (!DetectLayout()) {
    Fail("SchemaClassInfoData_t layout matches no known build (CEntityInstance round-trip failed)");
    return false;
  }
  SetDetail(g_layout.size == kCurrentLayout.size ? "verified (current class-info layout)" : "verified (legacy class-info layout)");
  if (DebugEnabled()) Print("schema: SchemaSystem_001=%p server scope=%p\n", g_schema, g_serverScope);
  g_status.store(1);
  return true;
}

int SchemaStatus(std::string* detail) {
  if (g_status.load() == 0) (void)SchemaInit();
  if (detail) {
    std::lock_guard<std::mutex> lk(g_mu);
    *detail = g_detail;
  }
  return g_status.load();
}

std::optional<int> SchemaFindOffset(const std::string& module,
                                    const std::string& className,
                                    const std::string& fieldName) {
  if (className.empty() || fieldName.empty()) return std::nullopt;
  // Only the server scope is supported (it also resolves entity2 classes such as CEntityIdentity).
  if (module != "server" && module != "libserver.so" && module != "server.dll") return std::nullopt;
  if (!SchemaInit()) return std::nullopt;

  const std::string key = className + "|" + fieldName;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_cache.find(key);
    if (it != g_cache.end()) {
      if (it->second < 0) return std::nullopt;
      return it->second;
    }
  }

  // Fields declared on base classes are resolved by walking the base-class chain, so callers can
  // ask for e.g. ("CBasePlayerWeapon", "m_nFallbackPaintKit") even though it lives on CEconEntity.
  const void* cls = FindClass(className.c_str());
  std::optional<int> off = cls ? FindFieldRecursive(cls, fieldName.c_str(), 0) : std::nullopt;

  if (cls) {
    // Only cache when the class exists: a missing class may just mean bindings aren't installed yet.
    std::lock_guard<std::mutex> lk(g_mu);
    g_cache[key] = off ? *off : -1;
  }
  Record(className, fieldName, off);
  if (!off && DebugEnabled()) {
    Print("schema: field not found: %s::%s%s\n", className.c_str(), fieldName.c_str(), cls ? "" : " (class not found)");
  }
  return off;
}

std::optional<int> SchemaClassSize(const std::string& className) {
  if (className.empty() || !SchemaInit()) return std::nullopt;
  const void* cls = FindClass(className.c_str());
  std::optional<int> sz;
  if (cls) sz = ReadField<int32_t>(cls, g_layout.size);
  Record(className, "sizeof", sz);
  return sz;
}

std::vector<SchemaLookup> SchemaLookupReport() {
  std::vector<SchemaLookup> out;
  std::lock_guard<std::mutex> lk(g_mu);
  for (const auto& kv : g_lookups) {
    SchemaLookup l;
    l.cls = kv.first.first;
    l.field = kv.first.second;
    if (kv.second >= 0) l.offset = kv.second;
    out.push_back(std::move(l));
  }
  return out;
}

}  // namespace readyup
