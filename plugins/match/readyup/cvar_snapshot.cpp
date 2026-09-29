// Engine-free half of cvar_snapshot.h: the snapshot itself, restore commands, JSON.
#include "readyup/cvar_snapshot.h"

#include "readyup/status_snapshot.h"

#include <cctype>

namespace readyup::cvar_snapshot {
namespace {

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

bool QuotableValue(const std::string& value) {
  for (unsigned char c : value) {
    if (c == '"' || c == ';' || c < 0x20 || c == 0x7f) return false;
  }
  return true;
}

std::vector<std::string> Snapshot::TakeKeysToQuery(const std::vector<std::string>& keys) {
  std::vector<std::string> out;
  for (const auto& k : keys) {
    const std::string n = Lower(k);
    if (n.empty() || values_.count(n) || asked_.count(n)) continue;
    asked_.insert(n);
    out.push_back(n);
  }
  return out;
}

void Snapshot::Record(const std::string& name, const char* value) {
  if (!value) return;
  const std::string n = Lower(name);
  if (n.empty()) return;
  asked_.insert(n);
  values_.emplace(n, value);  // first value wins
}

void Snapshot::Unask(const std::string& name) {
  const std::string n = Lower(name);
  if (!values_.count(n)) asked_.erase(n);
}

std::vector<std::string> Snapshot::RestoreCommands(std::vector<std::string>* skipped) const {
  std::vector<std::string> out;
  for (const auto& kv : values_) {
    if (!QuotableValue(kv.second)) {
      if (skipped) skipped->push_back(kv.first);
      continue;
    }
    out.push_back(kv.first + " \"" + kv.second + "\"");
  }
  return out;
}

std::string Snapshot::ToJson(bool withAsked) const {
  status::Json j = status::Json::Object();
  status::Json v = status::Json::Object();
  for (const auto& kv : values_) v[kv.first] = kv.second;
  j["values"] = std::move(v);
  if (withAsked) {
    status::Json a = status::Json::Array();
    for (const auto& n : asked_) a.Push(n);
    j["asked"] = std::move(a);
  }
  return j.Dump();
}

bool Snapshot::FromJson(const std::string& json, bool withAsked) {
  status::Json j;
  if (!status::Json::Parse(json, &j) || !j.IsObject()) return false;
  Clear();
  if (const status::Json* v = j.Find("values"); v && v->IsObject()) {
    for (const auto& kv : v->Members()) {
      if (kv.second.type() == status::Json::Type::String) values_[Lower(kv.first)] = kv.second.AsString();
    }
  }
  for (const auto& kv : values_) asked_.insert(kv.first);
  if (withAsked) {
    if (const status::Json* a = j.Find("asked")) {
      for (const auto& n : a->Items()) {
        if (n.type() == status::Json::Type::String && !n.AsString().empty()) asked_.insert(Lower(n.AsString()));
      }
    }
  }
  return true;
}

}  // namespace readyup::cvar_snapshot
