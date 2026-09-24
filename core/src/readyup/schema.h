#pragma once

#include <optional>
#include <string>
#include <vector>

namespace readyup {

// SchemaSystem helper (Source2 netvar offsets by class/field name).
//
// Only trusted once verified: SchemaSystem_001 and the libserver.so type scope must carry the
// expected RTTI (engine-surface rtti CSchemaSystem / CSchemaSystemTypeScope) before their virtual
// slots are called, and the SchemaClassInfoData_t layout must round-trip on CEntityInstance.
// Otherwise every lookup returns nullopt and the features that need schema offsets disable
// themselves (no guessed offsets).
bool SchemaInit();

// 0 = not available yet (schemasystem/bindings not loaded), 1 = verified, 2 = failed verification.
int SchemaStatus(std::string* detail);

// Attempts to find the byte offset of a field within a class (walks base classes).
// Example: SchemaFindOffset("server", "CCSPlayerController", "m_iKills");
std::optional<int> SchemaFindOffset(const std::string& module,
                                   const std::string& className,
                                   const std::string& fieldName);

// sizeof(class) per schema, or nullopt.
std::optional<int> SchemaClassSize(const std::string& className);

// Every class/field looked up so far (for `ru selftest`), sorted.
struct SchemaLookup {
  std::string cls;
  std::string field;
  std::optional<int> offset;  // nullopt = not found
};
std::vector<SchemaLookup> SchemaLookupReport();

}  // namespace readyup
