#include "midas_rules.h"

#include <cctype>
#include <cstdlib>
#include <vector>

namespace midas {
namespace {

std::string Lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string Trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

}  // namespace

std::set<uint64_t> ParseSteamIds(const std::string& text, int* bad) {
  std::set<uint64_t> out;
  int nbad = 0;
  std::string cur;
  auto flush = [&] {
    if (cur.empty()) return;
    bool digits = cur.size() == 17 && cur.compare(0, 7, "7656119") == 0;
    for (unsigned char c : cur) digits = digits && std::isdigit(c);
    if (digits) out.insert(std::strtoull(cur.c_str(), nullptr, 10));
    else ++nbad;
    cur.clear();
  };
  for (char c : text) {
    if (c == ',' || c == ';' || std::isspace(static_cast<unsigned char>(c))) flush();
    else cur.push_back(c);
  }
  flush();
  if (bad) *bad = nbad;
  return out;
}

bool ParseColor(const std::string& text, Rgba* out) {
  std::vector<int> v;
  std::string cur;
  const std::string t = Trim(text) + ",";
  for (char c : t) {
    if (c == ',') {
      const std::string p = Trim(cur);
      if (p.empty() || p.size() > 3) return false;
      for (unsigned char d : p) {
        if (!std::isdigit(d)) return false;
      }
      const int n = std::atoi(p.c_str());
      if (n > 255) return false;
      v.push_back(n);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (v.size() != 3 && v.size() != 4) return false;
  if (out) {
    out->r = static_cast<uint8_t>(v[0]);
    out->g = static_cast<uint8_t>(v[1]);
    out->b = static_cast<uint8_t>(v[2]);
    out->a = static_cast<uint8_t>(v.size() == 4 ? v[3] : 255);
  }
  return true;
}

bool ParseBool(const std::string& text, bool def) {
  const std::string t = Lower(Trim(text));
  if (t == "1" || t == "true" || t == "yes" || t == "on") return true;
  if (t == "0" || t == "false" || t == "no" || t == "off") return false;
  return def;
}

bool Active(bool enabled, const std::string& ruleset) { return enabled && Lower(Trim(ruleset)) != "valve"; }

bool ShouldTint(bool active, const std::set<uint64_t>& midas, uint64_t owner) {
  return active && owner != 0 && midas.count(owner) != 0;
}

}  // namespace midas
