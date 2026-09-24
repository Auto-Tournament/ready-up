// Small JSON value + parser + writer for the fleet link. Unlike libs/readyup/minijson it keeps
// object key order, decodes \uXXXX (incl. surrogate pairs) to UTF-8 and keeps integers exact
// (seq, ts and epoch are int64), so payloads round-trip unchanged.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fleet::json {

struct Value {
  enum class T { Null, Bool, Int, Num, Str, Obj, Arr };
  T t = T::Null;
  bool b = false;
  int64_t i = 0;
  double n = 0.0;
  std::string s;
  std::vector<std::pair<std::string, Value>> o;
  std::vector<Value> a;

  static Value Null() { return Value(); }
  static Value Bool(bool v) {
    Value x;
    x.t = T::Bool;
    x.b = v;
    return x;
  }
  static Value Int(int64_t v) {
    Value x;
    x.t = T::Int;
    x.i = v;
    return x;
  }
  static Value Num(double v) {
    Value x;
    x.t = T::Num;
    x.n = v;
    return x;
  }
  static Value Str(std::string v) {
    Value x;
    x.t = T::Str;
    x.s = std::move(v);
    return x;
  }
  static Value Object() {
    Value x;
    x.t = T::Obj;
    return x;
  }
  static Value Array() {
    Value x;
    x.t = T::Arr;
    return x;
  }

  bool IsNull() const { return t == T::Null; }
  bool IsObj() const { return t == T::Obj; }
  bool IsArr() const { return t == T::Arr; }
  bool IsStr() const { return t == T::Str; }
  bool IsNum() const { return t == T::Int || t == T::Num; }

  const Value* Get(std::string_view key) const;
  Value* Get(std::string_view key);
  // Sets (replaces or appends) a key; returns *this for chaining. Only on objects.
  Value& Set(std::string_view key, Value v);
  Value& Push(Value v);

  int64_t AsInt(int64_t def = 0) const;
  double AsNum(double def = 0.0) const;
  std::string AsStr(std::string def = {}) const;
  bool AsBool(bool def = false) const;
};

// Parses exactly one JSON value (surrounding whitespace allowed). maxDepth bounds nesting.
bool Parse(std::string_view in, Value* out, std::string* err = nullptr, int maxDepth = 64);

// Compact JSON. Never emits raw control characters or newlines.
std::string Dump(const Value& v);
void DumpTo(const Value& v, std::string& out);
void AppendQuoted(std::string& out, std::string_view s);

}  // namespace fleet::json
