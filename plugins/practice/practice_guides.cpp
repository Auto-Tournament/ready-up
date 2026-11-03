#include "practice_guides.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>

namespace practice {
namespace {

// The KV3 the annotation files use: `key = value`, objects in { }, arrays in [ ], quoted strings
// (with \n escapes), numbers, true / false, `//` and `<!-- -->` comments. Enough for these files.
struct Kv {
  enum class T { Null, Str, Num, Bool, Arr, Obj } t = T::Null;
  std::string s;
  double n = 0;
  bool b = false;
  std::vector<Kv> arr;
  std::vector<std::pair<std::string, Kv>> obj;

  const Kv* Get(const std::string& k) const {
    for (const auto& kv : obj) {
      if (kv.first == k) return &kv.second;
    }
    return nullptr;
  }
  std::string Str(const std::string& k) const {
    const Kv* v = Get(k);
    return v && v->t == T::Str ? v->s : "";
  }
  bool Bool(const std::string& k, bool def) const {
    const Kv* v = Get(k);
    return v && v->t == T::Bool ? v->b : def;
  }
  bool Vec(const std::string& k, Vec3f* out) const {
    const Kv* v = Get(k);
    if (!v || v->t != T::Arr || v->arr.size() < 3) return false;
    for (const Kv& e : v->arr) {
      if (e.t != T::Num) return false;
    }
    *out = Vec3f{static_cast<float>(v->arr[0].n), static_cast<float>(v->arr[1].n), static_cast<float>(v->arr[2].n)};
    return true;
  }
  std::string Text(const std::string& k) const {  // Title = { Text = "..." }
    const Kv* v = Get(k);
    return v && v->t == T::Obj ? v->Str("Text") : "";
  }
};

class Parser {
 public:
  explicit Parser(const std::string& s) : s_(s) {}

  bool Root(Kv* out) {
    Skip();
    if (!Value(out, 0) || out->t != Kv::T::Obj) return Fail("the file is not a KV3 object");
    return true;
  }
  std::string error;

 private:
  bool Fail(const std::string& m) {
    if (error.empty()) error = m + " (offset " + std::to_string(i_) + ")";
    return false;
  }
  void Skip() {
    for (;;) {
      while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_;
      if (s_.compare(i_, 4, "<!--") == 0) {
        const size_t e = s_.find("-->", i_);
        i_ = e == std::string::npos ? s_.size() : e + 3;
      } else if (s_.compare(i_, 2, "//") == 0) {
        while (i_ < s_.size() && s_[i_] != '\n') ++i_;
      } else {
        return;
      }
    }
  }
  bool String(std::string* out) {
    ++i_;  // the opening quote
    while (i_ < s_.size() && s_[i_] != '"') {
      char c = s_[i_++];
      if (c == '\\' && i_ < s_.size()) {
        const char e = s_[i_++];
        c = e == 'n' ? '\n' : e == 't' ? '\t' : e;
      }
      *out += c;
    }
    if (i_ >= s_.size()) return Fail("a string does not end");
    ++i_;
    return true;
  }
  bool Value(Kv* out, int depth) {
    if (depth > 32) return Fail("nested too deep");
    Skip();
    if (i_ >= s_.size()) return Fail("the file ends early");
    const char c = s_[i_];
    if (c == '{') {
      ++i_;
      out->t = Kv::T::Obj;
      for (;;) {
        Skip();
        if (i_ < s_.size() && s_[i_] == '}') {
          ++i_;
          return true;
        }
        std::string key;
        while (i_ < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[i_])) || s_[i_] == '_' || s_[i_] == '.')) key += s_[i_++];
        if (key.empty() && i_ < s_.size() && s_[i_] == '"' && !String(&key)) return false;
        if (key.empty()) return Fail("a key was expected");
        Skip();
        if (i_ >= s_.size() || s_[i_] != '=') return Fail("'=' was expected after " + key);
        ++i_;
        Kv v;
        if (!Value(&v, depth + 1)) return false;
        out->obj.emplace_back(std::move(key), std::move(v));
        Skip();
        if (i_ < s_.size() && s_[i_] == ',') ++i_;
      }
    }
    if (c == '[') {
      ++i_;
      out->t = Kv::T::Arr;
      for (;;) {
        Skip();
        if (i_ < s_.size() && s_[i_] == ']') {
          ++i_;
          return true;
        }
        Kv v;
        if (!Value(&v, depth + 1)) return false;
        out->arr.push_back(std::move(v));
        Skip();
        if (i_ < s_.size() && s_[i_] == ',') ++i_;
      }
    }
    if (c == '"') {
      out->t = Kv::T::Str;
      return String(&out->s);
    }
    std::string word;
    while (i_ < s_.size() && !std::isspace(static_cast<unsigned char>(s_[i_])) && s_[i_] != ',' && s_[i_] != ']' && s_[i_] != '}') {
      word += s_[i_++];
    }
    if (word == "true" || word == "false") {
      out->t = Kv::T::Bool;
      out->b = word == "true";
      return true;
    }
    if (word == "null") return true;
    char* end = nullptr;
    out->n = std::strtod(word.c_str(), &end);
    if (word.empty() || !end || *end) return Fail("a value was expected, got '" + word + "'");
    out->t = Kv::T::Num;
    return true;
  }

  const std::string& s_;
  size_t i_ = 0;
};

// "runthrow \n\n aim to the left of center" -> "runthrow, aim to the left of center".
std::string OneLine(const std::string& s) {
  std::string out;
  int gap = 0;  // 1: a space before the next word, 2: a line break (", ")
  for (char c : s) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      gap = std::max(gap, c == '\n' ? 2 : 1);
      continue;
    }
    if (!out.empty() && gap) out += gap == 2 && out.back() != ',' && out.back() != '.' ? ", " : " ";
    gap = 0;
    out += c;
  }
  return out;
}

}  // namespace

std::string GuideSlug(const std::string& title) {
  std::string out;
  for (char c : title) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (std::isalnum(u)) {
      out += static_cast<char>(std::tolower(u));
    } else if (!out.empty() && out.back() != '-') {
      out += '-';
    }
  }
  while (!out.empty() && out.back() == '-') out.pop_back();
  if (out.size() > 32) out.resize(32);
  while (!out.empty() && out.back() == '-') out.pop_back();
  return out;
}

std::string GuideKind(const std::string& t) {
  if (t == "smoke") return "smoke";
  if (t == "flash" || t == "flashbang") return "flash";
  if (t == "molotov" || t == "incendiary") return "molotov";
  if (t == "he" || t == "hegrenade") return "hegrenade";
  if (t == "decoy") return "decoy";
  return "";
}

std::string GuideThrow(const std::string& desc, bool jumpFlag) {
  std::string s;
  for (char c : desc.substr(0, desc.find('\n'))) s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  const auto has = [&](const char* w) { return s.find(w) != std::string::npos; };
  const bool jump = jumpFlag || has("jump");
  const bool left = has("lmb") || has("left click"), right = has("rmb") || has("right click");
  const std::string mod = has("run") ? "W + " : has("shift") || has("walk") ? "Shift + " : has("step") ? "Step + " : "";
  std::string base;
  if (jump) base = left && right ? "Middle jumpthrow" : right ? "Right-click jumpthrow" : "Jumpthrow";
  else if (has("throw") || left || right || !mod.empty()) base = left && right ? "Middle throw" : right ? "Right-click throw" : "Throw";
  if (base.empty()) return "";  // only where to aim
  if (!mod.empty()) base[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(base[0])));
  return mod + base;
}

bool ParseGuideFile(const std::string& text, GuideFile* out, std::string* error) {
  *out = GuideFile{};
  Kv root;
  Parser p(text);
  if (!p.Root(&root)) {
    if (error) *error = p.error;
    return false;
  }
  out->map = root.Str("MapName");
  // The aim of each main node (its first aim_target).
  std::map<std::string, const Kv*> aimOf;
  for (const auto& kv : root.obj) {
    const Kv& n = kv.second;
    if (kv.first.rfind("MapAnnotationNode", 0) != 0 || n.t != Kv::T::Obj) continue;
    if (n.Str("Type") == "grenade" && n.Str("SubType") == "aim_target" && n.Bool("Enabled", true)) {
      aimOf.emplace(n.Str("MasterNodeId"), &n);
    }
  }
  std::set<std::string> names;
  for (const auto& kv : root.obj) {
    const Kv& n = kv.second;
    if (kv.first.rfind("MapAnnotationNode", 0) != 0 || n.t != Kv::T::Obj) continue;
    if (n.Str("Type") != "grenade" || n.Str("SubType") != "main" || !n.Bool("Enabled", true)) continue;
    Lineup l;
    l.title = OneLine(n.Text("Title"));
    l.kind = GuideKind(n.Str("GrenadeType"));
    if (!n.Vec("Position", &l.pos) || l.title.empty()) {
      ++out->skipped;
      continue;
    }
    const auto a = aimOf.find(n.Str("Id"));
    std::string raw;
    if (a != aimOf.end() && a->second->Vec("Angles", &l.ang)) {
      raw = a->second->Text("Desc");
    } else if (!n.Vec("Angles", &l.ang)) {
      ++out->skipped;
      continue;
    }
    l.desc = OneLine(raw);
    l.how = GuideThrow(raw, n.Bool("JumpThrow", false));
    l.ang.z = 0;
    if (n.Bool("JumpThrow", false) && l.desc.find("jump") == std::string::npos) l.desc = "jumpthrow" + (l.desc.empty() ? "" : ", " + l.desc);
    // Unique names: the same title twice (one per side) gets -2, -3.
    std::string base = GuideSlug(l.title);
    if (base.empty()) base = "guide";
    l.name = base;
    for (int k = 2; !names.insert(l.name).second; ++k) {
      const std::string suffix = "-" + std::to_string(k);
      l.name = base.substr(0, 32 - suffix.size()) + suffix;
    }
    out->lineups.push_back(std::move(l));
  }
  return true;
}

}  // namespace practice
