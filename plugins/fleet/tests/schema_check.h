// A small JSON Schema (draft 2020-12 subset) validator, enough for the platform's fleet protocol
// schemas in plugins/fleet/protocol/v1 (copied from the platform repo, FLEET.md D18). Tests use
// it to check every frame fleet.so sends. Supported keywords: $ref (relative URIs + JSON
// pointers), type, const, enum, required, properties, additionalProperties, pattern,
// minLength/maxLength, minimum/maximum, items, minItems/maxItems, uniqueItems, maxProperties,
// propertyNames, oneOf, anyOf, allOf.
// Annotations (title, description, ...) are ignored. Not for production use.
#pragma once

#include "fleet_json.h"

#include <map>
#include <string>
#include <vector>

namespace schema {

class Set {
 public:
  // Loads every *.json under dir (recursively), keyed by "$id".
  bool LoadDir(const std::string& dir, std::string* err);
  bool Has(const std::string& id) const { return docs_.count(id) > 0; }
  // Validates `v` against the schema with this $id. Appends "path: problem" lines to errs.
  bool Validate(const fleet::json::Value& v, const std::string& id, std::vector<std::string>* errs) const;
  size_t size() const { return docs_.size(); }

 private:
  bool Check(const fleet::json::Value& v, const fleet::json::Value& s, const std::string& base,
             const std::string& path, std::vector<std::string>* errs, int depth) const;
  const fleet::json::Value* Resolve(const std::string& base, const std::string& ref, std::string* newBase) const;
  std::map<std::string, fleet::json::Value> docs_;
};

}  // namespace schema
