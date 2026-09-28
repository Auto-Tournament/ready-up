#include "practice_lineups.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <sstream>

namespace practice {
namespace {
using readyup::status::Json;

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool Finite3(const Vec3f& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

Json Vec(const Vec3f& v) {
  Json a = Json::Array();
  a.Push(static_cast<double>(v.x));
  a.Push(static_cast<double>(v.y));
  a.Push(static_cast<double>(v.z));
  return a;
}

bool ReadVec(const Json* j, Vec3f* out) {
  if (!j || j->type() != Json::Type::Array || j->Items().size() != 3) return false;
  float f[3];
  for (size_t i = 0; i < 3; ++i) {
    const Json& e = j->Items()[i];
    if (e.type() != Json::Type::Int && e.type() != Json::Type::Double) return false;
    f[i] = static_cast<float>(e.AsDouble());
  }
  *out = {f[0], f[1], f[2]};
  return Finite3(*out) && std::fabs(out->x) < 65536.f && std::fabs(out->y) < 65536.f && std::fabs(out->z) < 65536.f;
}

std::string Str(const Json* j) { return j && j->type() == Json::Type::String ? j->AsString() : std::string(); }

bool ParseFloat(std::string s, float* out) {
  s.erase(std::remove(s.begin(), s.end(), ','), s.end());
  if (s.empty()) return false;
  char* end = nullptr;
  const float v = std::strtof(s.c_str(), &end);
  if (!end || *end != '\0' || !std::isfinite(v)) return false;
  *out = v;
  return true;
}
}  // namespace

bool LineupBook::Add(const std::string& owner, const Lineup& l) {
  auto& m = byOwner_[owner];
  if (m.count(l.name)) return false;
  m[l.name] = l;
  return true;
}

bool LineupBook::Remove(const std::string& owner, const std::string& name) {
  auto it = byOwner_.find(owner);
  if (it == byOwner_.end() || !it->second.erase(name)) return false;
  if (it->second.empty()) byOwner_.erase(it);
  return true;
}

const Lineup* LineupBook::Find(const std::string& owner, const std::string& name) const {
  auto it = byOwner_.find(owner);
  if (it == byOwner_.end()) return nullptr;
  auto l = it->second.find(name);
  return l == it->second.end() ? nullptr : &l->second;
}

std::vector<const Lineup*> LineupBook::List(const std::string& owner, const std::string& filter) const {
  std::vector<const Lineup*> out;
  auto it = byOwner_.find(owner);
  if (it == byOwner_.end()) return out;
  const std::string f = Lower(filter);
  for (const auto& kv : it->second) {
    if (f.empty() || Lower(kv.first).find(f) != std::string::npos) out.push_back(&kv.second);
  }
  return out;
}

const Lineup* LineupBook::Load(const std::string& owner, const std::string& query, std::string* fromOwner) const {
  for (const std::string& o : {owner, std::string(kGlobalOwner)}) {
    auto it = byOwner_.find(o);
    if (it == byOwner_.end()) continue;
    std::vector<std::string> names;
    for (const auto& kv : it->second) names.push_back(kv.first);
    const std::string n = NearestName(query, names);
    if (n.empty()) continue;
    if (fromOwner) *fromOwner = o;
    return &it->second.at(n);
  }
  return nullptr;
}

size_t LineupBook::Count() const {
  size_t n = 0;
  for (const auto& kv : byOwner_) n += kv.second.size();
  return n;
}

Json LineupBook::ToJson() const {
  Json doc = Json::Object();
  Json& all = doc["lineups"];
  all = Json::Object();
  for (const auto& o : byOwner_) {
    Json& mine = all[o.first];
    mine = Json::Object();
    for (const auto& kv : o.second) {
      Json e = Json::Object();
      e["pos"] = Vec(kv.second.pos);
      e["ang"] = Vec(kv.second.ang);
      e["kind"] = kv.second.kind;
      e["desc"] = kv.second.desc;
      mine[kv.first] = e;
    }
  }
  return doc;
}

void LineupBook::FromJson(const Json& doc, int* skipped) {
  byOwner_.clear();
  int bad = 0;
  const Json* all = doc.Find("lineups");
  if (all && all->IsObject()) {
    for (const auto& o : all->Members()) {
      if (!o.second.IsObject()) {
        ++bad;
        continue;
      }
      for (const auto& kv : o.second.Members()) {
        Lineup l;
        l.name = kv.first;
        if (!ValidLineupName(l.name) || !kv.second.IsObject() || !ReadVec(kv.second.Find("pos"), &l.pos) ||
            !ReadVec(kv.second.Find("ang"), &l.ang)) {
          ++bad;
          continue;
        }
        l.kind = Str(kv.second.Find("kind"));
        l.desc = Str(kv.second.Find("desc")).substr(0, 128);
        byOwner_[o.first][l.name] = l;
      }
    }
  }
  if (skipped) *skipped = bad;
}

double DiceCoefficient(const std::string& a, const std::string& b) {
  auto bigrams = [](const std::string& s) {
    std::vector<std::string> out;
    for (size_t i = 0; i + 1 < s.size(); ++i) out.push_back(s.substr(i, 2));
    return out;
  };
  const auto ba = bigrams(a), bb = bigrams(b);
  if (ba.empty() && bb.empty()) return 0;
  // ME: bigrams1.Intersect(bigrams2).Count() (distinct common bigrams).
  const std::set<std::string> sa(ba.begin(), ba.end()), sb(bb.begin(), bb.end());
  int common = 0;
  for (const auto& g : sa) common += sb.count(g) ? 1 : 0;
  return 2.0 * common / static_cast<double>(ba.size() + bb.size());
}

std::string NearestName(const std::string& query, const std::vector<std::string>& names) {
  if (query.empty() || names.empty()) return {};
  const std::string q = Lower(query);
  for (const auto& n : names) {
    if (Lower(n) == q) return n;
  }
  if (q.size() == 1) {
    for (const auto& n : names) {
      if (!n.empty() && Lower(n)[0] == q[0]) return n;
    }
  }
  // ME took the best score whatever it was, so "nothingthere" loaded "window" (one shared bigram);
  // below 0.25 it is no match here.
  std::string best;
  double bestScore = 0.25 - 1e-9;
  for (const auto& n : names) {
    const double d = DiceCoefficient(q, Lower(n));
    if (d > bestScore) {
      bestScore = d;
      best = n;
    }
  }
  return best;
}

bool ParseImportCode(const std::string& code, std::string* name, Vec3f* pos, Vec3f* ang) {
  std::istringstream in(code);
  std::vector<std::string> parts;
  for (std::string w; in >> w;) {
    if (w == ",") continue;
    parts.push_back(w);
  }
  if (parts.size() != 7 || !ValidLineupName(parts[0])) return false;
  float f[6];
  for (size_t i = 0; i < 6; ++i) {
    if (!ParseFloat(parts[i + 1], &f[i])) return false;
  }
  *name = parts[0];
  *pos = {f[0], f[1], f[2]};
  *ang = {f[3], f[4], f[5]};
  return std::fabs(pos->x) < 65536.f && std::fabs(pos->y) < 65536.f && std::fabs(pos->z) < 65536.f;
}

std::string ExportCode(const Lineup& l) {
  char buf[192];
  std::snprintf(buf, sizeof(buf), "%s %.2f %.2f %.2f %.2f %.2f %.2f", l.name.c_str(), l.pos.x, l.pos.y, l.pos.z, l.ang.x,
                l.ang.y, l.ang.z);
  return buf;
}

bool ValidLineupName(const std::string& name) {
  if (name.empty() || name.size() > 32) return false;
  for (char c : name) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.') return false;
  }
  return true;
}

std::string MapFileStem(const std::string& map) {
  std::string out;
  for (char c : Lower(map)) {
    out += (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') ? c : '_';
    if (out.size() >= 64) break;
  }
  return out;
}

}  // namespace practice
