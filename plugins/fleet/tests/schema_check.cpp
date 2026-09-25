#include "schema_check.h"

#include "fleet_store.h"

#include <dirent.h>
#include <sys/stat.h>

#include <cmath>
#include <regex>
#include <set>

namespace schema {

using fleet::json::Value;

namespace {

void ListJson(const std::string& dir, std::vector<std::string>* out) {
  DIR* d = opendir(dir.c_str());
  if (!d) return;
  while (dirent* e = readdir(d)) {
    const std::string n = e->d_name;
    if (n == "." || n == "..") continue;
    const std::string p = dir + "/" + n;
    struct stat st {};
    if (stat(p.c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) ListJson(p, out);
    else if (n.size() > 5 && n.compare(n.size() - 5, 5, ".json") == 0) out->push_back(p);
  }
  closedir(d);
}

std::string DirOf(const std::string& uri) {
  const size_t p = uri.rfind('/');
  return p == std::string::npos ? std::string() : uri.substr(0, p + 1);
}

// Joins a relative reference onto a base URI's directory and folds "..".
std::string JoinUri(const std::string& base, const std::string& rel) {
  if (rel.find("://") != std::string::npos) return rel;
  std::string dir = DirOf(base);
  const size_t scheme = dir.find("://");
  const std::string prefix = scheme == std::string::npos ? "" : dir.substr(0, dir.find('/', scheme + 3));
  std::string path = dir.substr(prefix.size()) + rel;
  std::vector<std::string> parts;
  size_t i = 0;
  while (i <= path.size()) {
    size_t j = path.find('/', i);
    if (j == std::string::npos) j = path.size();
    const std::string seg = path.substr(i, j - i);
    if (seg == "..") {
      if (!parts.empty()) parts.pop_back();
    } else if (!seg.empty() && seg != ".") {
      parts.push_back(seg);
    }
    i = j + 1;
  }
  std::string out = prefix;
  for (const auto& p : parts) out += "/" + p;
  return out;
}

bool IsInteger(const Value& v) {
  return v.t == Value::T::Int || (v.t == Value::T::Num && std::floor(v.n) == v.n);
}

bool TypeIs(const Value& v, const std::string& t) {
  if (t == "object") return v.t == Value::T::Obj;
  if (t == "array") return v.t == Value::T::Arr;
  if (t == "string") return v.t == Value::T::Str;
  if (t == "boolean") return v.t == Value::T::Bool;
  if (t == "null") return v.t == Value::T::Null;
  if (t == "integer") return IsInteger(v);
  if (t == "number") return v.IsNum();
  return false;
}

size_t Utf8Len(const std::string& s) {
  size_t n = 0;
  for (unsigned char c : s) n += (c & 0xC0) != 0x80;
  return n;
}

bool Equal(const Value& a, const Value& b) {
  if (a.IsNum() && b.IsNum()) return a.AsNum() == b.AsNum();
  return fleet::json::Dump(a) == fleet::json::Dump(b);
}

}  // namespace

bool Set::LoadDir(const std::string& dir, std::string* err) {
  std::vector<std::string> files;
  ListJson(dir, &files);
  for (const auto& f : files) {
    std::string text;
    Value v;
    std::string perr;
    if (!fleet::ReadFile(f, &text) || !fleet::json::Parse(text, &v, &perr) || !v.IsObj()) {
      if (err) *err = f + ": " + perr;
      return false;
    }
    const Value* id = v.Get("$id");
    if (!id || !id->IsStr()) {
      if (err) *err = f + ": no $id";
      return false;
    }
    docs_[id->s] = std::move(v);
  }
  if (docs_.empty() && err) *err = "no schemas in " + dir;
  return !docs_.empty();
}

const Value* Set::Resolve(const std::string& base, const std::string& ref, std::string* newBase) const {
  const size_t hash = ref.find('#');
  const std::string docPart = hash == std::string::npos ? ref : ref.substr(0, hash);
  const std::string ptr = hash == std::string::npos ? "" : ref.substr(hash + 1);
  const std::string docId = docPart.empty() ? base : JoinUri(base, docPart);
  auto it = docs_.find(docId);
  if (it == docs_.end()) return nullptr;
  *newBase = docId;
  const Value* cur = &it->second;
  size_t i = 0;
  while (i < ptr.size()) {
    if (ptr[i] != '/') return nullptr;
    size_t j = ptr.find('/', i + 1);
    if (j == std::string::npos) j = ptr.size();
    const std::string key = ptr.substr(i + 1, j - i - 1);
    cur = cur->Get(key);
    if (!cur) return nullptr;
    i = j;
  }
  return cur;
}

bool Set::Validate(const Value& v, const std::string& id, std::vector<std::string>* errs) const {
  auto it = docs_.find(id);
  if (it == docs_.end()) {
    errs->push_back("no schema " + id);
    return false;
  }
  const size_t before = errs->size();
  Check(v, it->second, id, "$", errs, 0);
  return errs->size() == before;
}

bool Set::Check(const Value& v, const Value& s, const std::string& base, const std::string& path,
                std::vector<std::string>* errs, int depth) const {
  if (depth > 64) {
    errs->push_back(path + ": schema too deep");
    return false;
  }
  const size_t before = errs->size();
  auto fail = [&](const std::string& why) { errs->push_back(path + ": " + why); };
  if (!s.IsObj()) return true;

  if (const Value* ref = s.Get("$ref")) {
    std::string nb;
    const Value* target = Resolve(base, ref->AsStr(), &nb);
    if (!target) fail("unresolved $ref " + ref->AsStr());
    else Check(v, *target, nb, path, errs, depth + 1);
  }
  if (const Value* t = s.Get("type")) {
    bool ok = false;
    if (t->IsStr()) ok = TypeIs(v, t->s);
    for (const auto& x : t->a) ok = ok || TypeIs(v, x.AsStr());
    if (!ok) fail("type is not " + fleet::json::Dump(*t));
  }
  if (const Value* c = s.Get("const"); c && !Equal(v, *c)) fail("not const " + fleet::json::Dump(*c));
  if (const Value* e = s.Get("enum")) {
    bool ok = false;
    for (const auto& x : e->a) ok = ok || Equal(v, x);
    if (!ok) fail("not in enum " + fleet::json::Dump(*e));
  }
  if (v.IsStr()) {
    if (const Value* p = s.Get("pattern")) {
      try {
        if (!std::regex_search(v.s, std::regex(p->s, std::regex::ECMAScript))) fail("does not match " + p->s);
      } catch (const std::regex_error&) {
        fail("bad pattern " + p->s);
      }
    }
    if (const Value* m = s.Get("minLength"); m && Utf8Len(v.s) < static_cast<size_t>(m->AsInt())) fail("too short");
    if (const Value* m = s.Get("maxLength"); m && Utf8Len(v.s) > static_cast<size_t>(m->AsInt())) fail("too long");
  }
  if (v.IsNum()) {
    if (const Value* m = s.Get("minimum"); m && v.AsNum() < m->AsNum()) fail("below minimum");
    if (const Value* m = s.Get("maximum"); m && v.AsNum() > m->AsNum()) fail("above maximum");
  }
  if (v.IsObj()) {
    if (const Value* r = s.Get("required")) {
      for (const auto& k : r->a) {
        if (!v.Get(k.AsStr())) fail("missing " + k.AsStr());
      }
    }
    const Value* props = s.Get("properties");
    const Value* addl = s.Get("additionalProperties");
    for (const auto& kv : v.o) {
      const Value* ps = props ? props->Get(kv.first) : nullptr;
      if (ps) {
        Check(kv.second, *ps, base, path + "." + kv.first, errs, depth + 1);
      } else if (addl) {
        if (addl->t == Value::T::Bool && !addl->b) fail("unexpected property " + kv.first);
        else if (addl->IsObj()) Check(kv.second, *addl, base, path + "." + kv.first, errs, depth + 1);
      }
    }
    if (const Value* m = s.Get("maxProperties"); m && v.o.size() > static_cast<size_t>(m->AsInt())) fail("too many properties");
    if (const Value* pn = s.Get("propertyNames")) {
      for (const auto& kv : v.o) Check(Value::Str(kv.first), *pn, base, path + "{" + kv.first + "}", errs, depth + 1);
    }
  }
  if (v.IsArr()) {
    if (const Value* items = s.Get("items")) {
      for (size_t i = 0; i < v.a.size(); ++i) Check(v.a[i], *items, base, path + "[" + std::to_string(i) + "]", errs, depth + 1);
    }
    if (const Value* m = s.Get("maxItems"); m && v.a.size() > static_cast<size_t>(m->AsInt())) fail("too many items");
    if (const Value* m = s.Get("minItems"); m && v.a.size() < static_cast<size_t>(m->AsInt())) fail("too few items");
    if (const Value* u = s.Get("uniqueItems"); u && u->AsBool()) {
      std::set<std::string> seen;
      for (const auto& x : v.a) {
        if (!seen.insert(fleet::json::Dump(x)).second) fail("duplicate items");
      }
    }
  }
  if (const Value* any = s.Get("anyOf")) {
    bool ok = false;
    for (const auto& sub : any->a) {
      std::vector<std::string> tmp;
      Check(v, sub, base, path, &tmp, depth + 1);
      ok = ok || tmp.empty();
    }
    if (!ok) fail("anyOf matched no branch");
  }
  if (const Value* all = s.Get("allOf")) {
    for (const auto& sub : all->a) Check(v, sub, base, path, errs, depth + 1);
  }
  if (const Value* one = s.Get("oneOf")) {
    int matches = 0;
    for (const auto& sub : one->a) {
      std::vector<std::string> tmp;
      Check(v, sub, base, path, &tmp, depth + 1);
      matches += tmp.empty();
    }
    if (matches != 1) fail("oneOf matched " + std::to_string(matches) + " branches");
  }
  return errs->size() == before;
}

}  // namespace schema
