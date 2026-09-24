#pragma once

#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace readyup::minijson {

// Minimal JSON value tree (enough for Ready Up match config parsing).
struct Value {
  enum class Type { Null, Bool, Number, String, Object, Array };
  Type type = Type::Null;
  bool b = false;
  double num = 0.0;
  std::string str;
  std::unordered_map<std::string, Value> obj;
  std::vector<Value> arr;

  const Value* get(const char* key) const {
    if (type != Type::Object) return nullptr;
    auto it = obj.find(key);
    if (it == obj.end()) return nullptr;
    return &it->second;
  }
};

struct ParseError {
  std::string msg;
  size_t offset = 0;
};

std::optional<Value> Parse(const std::string& json, ParseError* err);

inline bool IsString(const Value* v) { return v && v->type == Value::Type::String; }
inline bool IsNumber(const Value* v) { return v && v->type == Value::Type::Number; }
inline bool IsObject(const Value* v) { return v && v->type == Value::Type::Object; }

inline std::optional<std::string> AsString(const Value* v) {
  if (!IsString(v)) return std::nullopt;
  return v->str;
}

inline std::optional<int64_t> AsInt(const Value* v) {
  if (!IsNumber(v)) return std::nullopt;
  return static_cast<int64_t>(v->num);
}

}  // namespace readyup::minijson

