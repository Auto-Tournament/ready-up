#include "essentials_rules.h"

#include "readyup/status_snapshot.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace essentials {
namespace {
using readyup::status::Json;

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
}  // namespace

uint64_t ParseSteamId64(const std::string& s) {
  if (s.size() != 17 || s.compare(0, 7, "7656119") != 0) return 0;
  for (unsigned char c : s) {
    if (!std::isdigit(c)) return 0;
  }
  return std::strtoull(s.c_str(), nullptr, 10);
}

std::vector<Admin> ParseAdmins(const std::string& json, bool* ok) {
  std::vector<Admin> out;
  if (ok) *ok = true;
  if (json.find_first_not_of(" \t\r\n") == std::string::npos) return out;
  Json doc;
  if (!Json::Parse(json, &doc) || !doc.IsObject()) {
    if (ok) *ok = false;
    return out;
  }
  const Json* arr = doc.Find("admins");
  if (!arr || arr->type() != Json::Type::Array) return out;
  for (const Json& a : arr->Items()) {
    Admin e;
    const Json* id = a.IsObject() ? a.Find("steamid64") : &a;
    if (id && id->type() == Json::Type::String) e.steamid64 = ParseSteamId64(id->AsString());
    if (a.IsObject()) {
      if (const Json* n = a.Find("name"); n && n->type() == Json::Type::String) e.name = n->AsString();
    }
    if (e.steamid64 && !IsAdmin(out, e.steamid64)) out.push_back(std::move(e));
  }
  return out;
}

std::string AdminsJson(const std::vector<Admin>& admins) {
  Json doc = Json::Object();
  doc["version"] = 1;
  Json arr = Json::Array();
  for (const auto& a : admins) {
    Json o = Json::Object();
    o["steamid64"] = std::to_string(a.steamid64);
    o["name"] = a.name;
    arr.Push(std::move(o));
  }
  doc["admins"] = std::move(arr);
  return doc.Dump() + "\n";
}

bool IsAdmin(const std::vector<Admin>& admins, uint64_t steamid64) {
  return steamid64 && std::any_of(admins.begin(), admins.end(), [&](const Admin& a) { return a.steamid64 == steamid64; });
}

bool AddAdmin(std::vector<Admin>* admins, uint64_t steamid64, const std::string& name) {
  if (!steamid64 || IsAdmin(*admins, steamid64)) return false;
  admins->push_back(Admin{steamid64, name});
  return true;
}

bool RemoveAdmin(std::vector<Admin>* admins, uint64_t steamid64) {
  const auto it = std::remove_if(admins->begin(), admins->end(), [&](const Admin& a) { return a.steamid64 == steamid64; });
  if (it == admins->end()) return false;
  admins->erase(it, admins->end());
  return true;
}

bool FindPlayer(const std::vector<Player>& players, const std::string& fragment, Player* out, std::string* err) {
  const std::string f = Lower(fragment);
  if (f.empty()) {
    if (err) *err = "give a SteamID64 or part of a connected player's name";
    return false;
  }
  std::vector<const Player*> hits;
  for (const auto& p : players) {
    const std::string n = Lower(p.name);
    if (n == f) {
      if (out) *out = p;
      return true;
    }
    if (n.find(f) != std::string::npos) hits.push_back(&p);
  }
  if (hits.size() == 1) {
    if (out) *out = *hits[0];
    return true;
  }
  if (err) *err = hits.empty() ? "no connected player matches \"" + fragment + "\""
                               : std::to_string(hits.size()) + " players match \"" + fragment + "\"; be more specific";
  return false;
}

std::string MapArgToEntry(const std::string& arg) {
  const std::string l = Lower(arg);
  if (l.find("steamcommunity.com/") == std::string::npos) return arg;
  size_t p = l.find("?id=");
  if (p == std::string::npos) p = l.find("&id=");
  if (p == std::string::npos) return arg;
  p += 4;
  size_t e = p;
  while (e < l.size() && std::isdigit(static_cast<unsigned char>(l[e]))) ++e;
  return e > p ? arg.substr(p, e - p) : arg;
}

bool MapCommandBlocked(const std::string& ruMode) {
  return ruMode == "match_live" || ruMode == "match_knife" || ruMode == "knife";
}

}  // namespace essentials
