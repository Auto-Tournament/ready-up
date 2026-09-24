#include "readyup/minijson.h"

#include <cerrno>
#include <cstdlib>

namespace readyup::minijson {
namespace {

struct P {
  const char* s = nullptr;
  size_t n = 0;
  size_t i = 0;
  ParseError* err = nullptr;

  void fail(const char* msg) {
    if (!err) return;
    err->msg = msg;
    err->offset = i;
  }

  bool eof() const { return i >= n; }
  char peek() const { return eof() ? '\0' : s[i]; }
  char get() { return eof() ? '\0' : s[i++]; }

  void skipWs() {
    while (!eof()) {
      const unsigned char c = static_cast<unsigned char>(s[i]);
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        ++i;
        continue;
      }
      break;
    }
  }
};

static bool parseValue(P& p, Value& out);

static bool parseLiteral(P& p, const char* lit) {
  const size_t start = p.i;
  for (size_t k = 0; lit[k]; ++k) {
    if (p.eof() || p.s[p.i] != lit[k]) {
      p.i = start;
      return false;
    }
    ++p.i;
  }
  return true;
}

static bool parseString(P& p, std::string& out) {
  if (p.get() != '"') return false;
  out.clear();
  while (!p.eof()) {
    char c = p.get();
    if (c == '"') return true;
    if (c == '\\') {
      if (p.eof()) return false;
      char e = p.get();
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
          // Minimal \uXXXX (BMP) support; only handles ASCII subset safely.
          // If non-ASCII, we keep it as '?' to avoid UTF-8 encoding complexity here.
          unsigned int code = 0;
          for (int k = 0; k < 4; ++k) {
            if (p.eof()) return false;
            char h = p.get();
            code <<= 4;
            if (h >= '0' && h <= '9') code |= static_cast<unsigned int>(h - '0');
            else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned int>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned int>(h - 'A' + 10);
            else return false;
          }
          if (code <= 0x7F) out.push_back(static_cast<char>(code));
          else out.push_back('?');
          break;
        }
        default:
          return false;
      }
      continue;
    }
    out.push_back(c);
  }
  return false;
}

static bool parseNumber(P& p, double& out) {
  const size_t start = p.i;
  if (p.peek() == '-') p.get();
  bool any = false;
  while (std::isdigit(static_cast<unsigned char>(p.peek())) != 0) {
    any = true;
    p.get();
  }
  if (!any) {
    p.i = start;
    return false;
  }
  if (p.peek() == '.') {
    p.get();
    while (std::isdigit(static_cast<unsigned char>(p.peek())) != 0) p.get();
  }
  if (p.peek() == 'e' || p.peek() == 'E') {
    p.get();
    if (p.peek() == '+' || p.peek() == '-') p.get();
    while (std::isdigit(static_cast<unsigned char>(p.peek())) != 0) p.get();
  }

  const std::string tmp(p.s + start, p.i - start);
  char* endp = nullptr;
  errno = 0;
  out = std::strtod(tmp.c_str(), &endp);
  if (errno != 0 || endp == tmp.c_str()) {
    p.i = start;
    return false;
  }
  return true;
}

static bool parseArray(P& p, Value& out) {
  if (p.get() != '[') return false;
  out.type = Value::Type::Array;
  out.arr.clear();
  p.skipWs();
  if (p.peek() == ']') {
    p.get();
    return true;
  }
  for (;;) {
    p.skipWs();
    Value v;
    if (!parseValue(p, v)) return false;
    out.arr.push_back(std::move(v));
    p.skipWs();
    const char c = p.get();
    if (c == ']') return true;
    if (c != ',') return false;
  }
}

static bool parseObject(P& p, Value& out) {
  if (p.get() != '{') return false;
  out.type = Value::Type::Object;
  out.obj.clear();
  p.skipWs();
  if (p.peek() == '}') {
    p.get();
    return true;
  }
  for (;;) {
    p.skipWs();
    if (p.peek() != '"') return false;
    std::string key;
    if (!parseString(p, key)) return false;
    p.skipWs();
    if (p.get() != ':') return false;
    p.skipWs();
    Value v;
    if (!parseValue(p, v)) return false;
    out.obj.emplace(std::move(key), std::move(v));
    p.skipWs();
    const char c = p.get();
    if (c == '}') return true;
    if (c != ',') return false;
  }
}

static bool parseValue(P& p, Value& out) {
  p.skipWs();
  const char c = p.peek();
  if (c == '\0') return false;
  if (c == '"') {
    out.type = Value::Type::String;
    return parseString(p, out.str);
  }
  if (c == '{') return parseObject(p, out);
  if (c == '[') return parseArray(p, out);
  if (c == 't') {
    if (!parseLiteral(p, "true")) return false;
    out.type = Value::Type::Bool;
    out.b = true;
    return true;
  }
  if (c == 'f') {
    if (!parseLiteral(p, "false")) return false;
    out.type = Value::Type::Bool;
    out.b = false;
    return true;
  }
  if (c == 'n') {
    if (!parseLiteral(p, "null")) return false;
    out.type = Value::Type::Null;
    return true;
  }
  double num = 0.0;
  if (parseNumber(p, num)) {
    out.type = Value::Type::Number;
    out.num = num;
    return true;
  }
  return false;
}

}  // namespace

std::optional<Value> Parse(const std::string& json, ParseError* err) {
  if (err) {
    err->msg.clear();
    err->offset = 0;
  }
  P p;
  p.s = json.data();
  p.n = json.size();
  p.i = 0;
  p.err = err;

  Value v;
  if (!parseValue(p, v)) {
    if (err && err->msg.empty()) p.fail("parse error");
    return std::nullopt;
  }
  p.skipWs();
  if (!p.eof()) {
    if (err && err->msg.empty()) p.fail("trailing characters");
    return std::nullopt;
  }
  return v;
}

}  // namespace readyup::minijson

