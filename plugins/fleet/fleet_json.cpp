#include "fleet_json.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fleet::json {

const Value* Value::Get(std::string_view key) const {
  if (t != T::Obj) return nullptr;
  for (const auto& kv : o) {
    if (kv.first == key) return &kv.second;
  }
  return nullptr;
}

Value* Value::Get(std::string_view key) {
  if (t != T::Obj) return nullptr;
  for (auto& kv : o) {
    if (kv.first == key) return &kv.second;
  }
  return nullptr;
}

Value& Value::Set(std::string_view key, Value v) {
  if (t != T::Obj) {
    *this = Object();
  }
  if (Value* cur = Get(key)) {
    *cur = std::move(v);
  } else {
    o.emplace_back(std::string(key), std::move(v));
  }
  return *this;
}

Value& Value::Push(Value v) {
  if (t != T::Arr) *this = Array();
  a.push_back(std::move(v));
  return *this;
}

int64_t Value::AsInt(int64_t def) const {
  if (t == T::Int) return i;
  if (t == T::Num && std::isfinite(n)) return static_cast<int64_t>(n);
  return def;
}

double Value::AsNum(double def) const {
  if (t == T::Num) return n;
  if (t == T::Int) return static_cast<double>(i);
  return def;
}

std::string Value::AsStr(std::string def) const { return t == T::Str ? s : def; }

bool Value::AsBool(bool def) const { return t == T::Bool ? b : def; }

namespace {

struct Parser {
  std::string_view in;
  size_t i = 0;
  int maxDepth = 64;
  std::string err;

  bool Fail(const char* what) {
    if (err.empty()) {
      char buf[96];
      std::snprintf(buf, sizeof(buf), "%s at offset %zu", what, i);
      err = buf;
    }
    return false;
  }
  void Ws() {
    while (i < in.size() && (in[i] == ' ' || in[i] == '\t' || in[i] == '\n' || in[i] == '\r')) ++i;
  }
  bool Lit(const char* w) {
    const size_t n = std::strlen(w);
    if (in.substr(i, n) != w) return Fail("bad literal");
    i += n;
    return true;
  }
  static void PutUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  bool Hex4(uint32_t* out) {
    if (i + 4 > in.size()) return Fail("short \\u escape");
    uint32_t v = 0;
    for (int k = 0; k < 4; ++k) {
      const char h = in[i++];
      v <<= 4;
      if (h >= '0' && h <= '9') v |= static_cast<uint32_t>(h - '0');
      else if (h >= 'a' && h <= 'f') v |= static_cast<uint32_t>(h - 'a' + 10);
      else if (h >= 'A' && h <= 'F') v |= static_cast<uint32_t>(h - 'A' + 10);
      else return Fail("bad \\u escape");
    }
    *out = v;
    return true;
  }
  bool Str(std::string& out) {
    if (i >= in.size() || in[i] != '"') return Fail("expected string");
    ++i;
    out.clear();
    while (i < in.size()) {
      const char c = in[i++];
      if (c == '"') return true;
      if (static_cast<unsigned char>(c) < 0x20) return Fail("control character in string");
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (i >= in.size()) break;
      const char e = in[i++];
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          uint32_t cp = 0;
          if (!Hex4(&cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            uint32_t lo = 0;
            if (i + 2 <= in.size() && in[i] == '\\' && in[i + 1] == 'u') {
              i += 2;
              if (!Hex4(&lo)) return false;
              if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              } else {
                PutUtf8(out, 0xFFFD);
                cp = lo;
              }
            } else {
              cp = 0xFFFD;
            }
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = 0xFFFD;
          }
          PutUtf8(out, cp);
          break;
        }
        default:
          return Fail("bad escape");
      }
    }
    return Fail("unterminated string");
  }
  bool Num(Value& out) {
    const size_t start = i;
    bool isFloat = false;
    if (i < in.size() && in[i] == '-') ++i;
    const size_t digits = i;
    while (i < in.size() && in[i] >= '0' && in[i] <= '9') ++i;
    if (i == digits) return Fail("bad number");
    if (i < in.size() && in[i] == '.') {
      isFloat = true;
      ++i;
      const size_t f = i;
      while (i < in.size() && in[i] >= '0' && in[i] <= '9') ++i;
      if (i == f) return Fail("bad number");
    }
    if (i < in.size() && (in[i] == 'e' || in[i] == 'E')) {
      isFloat = true;
      ++i;
      if (i < in.size() && (in[i] == '+' || in[i] == '-')) ++i;
      const size_t e = i;
      while (i < in.size() && in[i] >= '0' && in[i] <= '9') ++i;
      if (i == e) return Fail("bad number");
    }
    const std::string tmp(in.substr(start, i - start));
    errno = 0;
    if (!isFloat) {
      char* end = nullptr;
      const long long v = std::strtoll(tmp.c_str(), &end, 10);
      if (errno == 0 && end && *end == '\0') {
        out = Value::Int(v);
        return true;
      }
      errno = 0;
    }
    char* end = nullptr;
    const double d = std::strtod(tmp.c_str(), &end);
    if (!end || *end != '\0') return Fail("bad number");
    out = Value::Num(d);
    return true;
  }
  bool Val(Value& out, int depth) {
    if (depth > maxDepth) return Fail("nesting too deep");
    Ws();
    if (i >= in.size()) return Fail("unexpected end");
    const char c = in[i];
    if (c == '{') {
      ++i;
      out = Value::Object();
      Ws();
      if (i < in.size() && in[i] == '}') {
        ++i;
        return true;
      }
      for (;;) {
        Ws();
        std::string k;
        if (!Str(k)) return false;
        Ws();
        if (i >= in.size() || in[i] != ':') return Fail("expected ':'");
        ++i;
        Value v;
        if (!Val(v, depth + 1)) return false;
        out.Set(k, std::move(v));  // duplicate keys: last one wins
        Ws();
        if (i < in.size() && in[i] == ',') {
          ++i;
          continue;
        }
        if (i < in.size() && in[i] == '}') {
          ++i;
          return true;
        }
        return Fail("expected ',' or '}'");
      }
    }
    if (c == '[') {
      ++i;
      out = Value::Array();
      Ws();
      if (i < in.size() && in[i] == ']') {
        ++i;
        return true;
      }
      for (;;) {
        Value v;
        if (!Val(v, depth + 1)) return false;
        out.a.push_back(std::move(v));
        Ws();
        if (i < in.size() && in[i] == ',') {
          ++i;
          continue;
        }
        if (i < in.size() && in[i] == ']') {
          ++i;
          return true;
        }
        return Fail("expected ',' or ']'");
      }
    }
    if (c == '"') {
      out = Value::Str({});
      return Str(out.s);
    }
    if (c == 't') {
      out = Value::Bool(true);
      return Lit("true");
    }
    if (c == 'f') {
      out = Value::Bool(false);
      return Lit("false");
    }
    if (c == 'n') {
      out = Value::Null();
      return Lit("null");
    }
    return Num(out);
  }
};

}  // namespace

bool Parse(std::string_view in, Value* out, std::string* err, int maxDepth) {
  Parser p;
  p.in = in;
  p.maxDepth = maxDepth;
  Value v;
  bool ok = p.Val(v, 0);
  if (ok) {
    p.Ws();
    if (p.i != in.size()) ok = p.Fail("trailing data");
  }
  if (!ok) {
    if (err) *err = p.err;
    return false;
  }
  if (out) *out = std::move(v);
  return true;
}

void AppendQuoted(std::string& out, std::string_view s) {
  out.push_back('"');
  for (const char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20 || c == 0x7F) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(ch);
        }
    }
  }
  out.push_back('"');
}

void DumpTo(const Value& v, std::string& out) {
  switch (v.t) {
    case Value::T::Null: out += "null"; break;
    case Value::T::Bool: out += v.b ? "true" : "false"; break;
    case Value::T::Int: out += std::to_string(v.i); break;
    case Value::T::Num: {
      if (!std::isfinite(v.n)) {
        out += "null";
        break;
      }
      char buf[40];
      std::snprintf(buf, sizeof(buf), "%.17g", v.n);
      out += buf;
      break;
    }
    case Value::T::Str: AppendQuoted(out, v.s); break;
    case Value::T::Obj: {
      out.push_back('{');
      bool first = true;
      for (const auto& kv : v.o) {
        if (!first) out.push_back(',');
        first = false;
        AppendQuoted(out, kv.first);
        out.push_back(':');
        DumpTo(kv.second, out);
      }
      out.push_back('}');
      break;
    }
    case Value::T::Arr: {
      out.push_back('[');
      for (size_t k = 0; k < v.a.size(); ++k) {
        if (k) out.push_back(',');
        DumpTo(v.a[k], out);
      }
      out.push_back(']');
      break;
    }
  }
}

std::string Dump(const Value& v) {
  std::string out;
  DumpTo(v, out);
  return out;
}

}  // namespace fleet::json
