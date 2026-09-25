// Map entries and loaded map names (workshop maps). See map_names.h.
#include "readyup/map_names.h"

#include <cctype>
#include <map>
#include <mutex>

namespace readyup::mapnames {
namespace {

std::mutex g_mu;
std::map<std::string, std::string> g_bound;  // workshop id -> map name
std::string g_pending;                       // id of the last host_workshop_map not loaded yet

bool IsDigits(const std::string& s) {
  if (s.empty() || s.size() > 20) return false;
  for (unsigned char c : s) {
    if (!std::isdigit(c)) return false;
  }
  return true;
}

bool SafeName(const std::string& s) {
  if (s.empty() || s.size() > 128) return false;
  for (unsigned char c : s) {
    if (!(std::isalnum(c) || c == '_' || c == '/' || c == '.' || c == '-')) return false;
  }
  return s.find("..") == std::string::npos && s.front() != '/' && s.back() != '/';
}

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool StartsWith(const std::string& s, const char* prefix) {
  const std::string p = prefix;
  return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

}  // namespace

bool ParseEntry(const std::string& entry, MapRef* out) {
  MapRef r;
  if (StartsWith(entry, "ws:")) {
    r.workshop_id = entry.substr(3);
    if (!IsDigits(r.workshop_id)) return false;
  } else if (StartsWith(entry, "workshop/")) {
    const std::string rest = entry.substr(9);
    const size_t slash = rest.find('/');
    r.workshop_id = rest.substr(0, slash);
    if (!IsDigits(r.workshop_id)) return false;
    if (slash != std::string::npos) {
      r.name = rest.substr(slash + 1);
      if (!SafeName(r.name)) return false;
    }
  } else if (!entry.empty() && entry.find_first_not_of("0123456789") == std::string::npos) {
    r.workshop_id = entry;  // all digits: a workshop id (at most 20 of them)
    if (!IsDigits(entry)) return false;
  } else {
    if (!SafeName(entry)) return false;
    r.name = entry;
  }
  if (out) *out = std::move(r);
  return true;
}

std::string MakeEntry(const std::string& name, const std::string& workshopId) {
  MapRef r;
  const bool nameOk = !name.empty() && ParseEntry(name, &r);
  if (workshopId.empty()) return nameOk ? name : std::string();
  if (!IsDigits(workshopId)) return {};
  if (!nameOk || r.name.empty()) return "workshop/" + workshopId;
  return "workshop/" + workshopId + "/" + r.name;
}

std::string LoadCommand(const std::string& entry) {
  MapRef r;
  if (!ParseEntry(entry, &r)) return {};
  if (!r.workshop_id.empty()) return "host_workshop_map " + r.workshop_id;
  return "changelevel " + r.name;
}

std::string LoadedBaseName(const std::string& loaded) {
  std::string s = loaded;
  if (StartsWith(s, "workshop/")) {
    const size_t slash = s.find('/', 9);
    s = slash == std::string::npos ? std::string() : s.substr(slash + 1);
  }
  const size_t last = s.find_last_of("/\\");
  if (last != std::string::npos) s = s.substr(last + 1);
  for (const char* ext : {".vpk", ".bsp"}) {
    const size_t n = std::char_traits<char>::length(ext);
    if (s.size() > n && Lower(s.substr(s.size() - n)) == ext) s.resize(s.size() - n);
  }
  return s;
}

std::string WorkshopIdOfLoaded(const std::string& loaded) {
  if (!StartsWith(loaded, "workshop/")) return {};
  const size_t slash = loaded.find('/', 9);
  const std::string id = loaded.substr(9, slash == std::string::npos ? std::string::npos : slash - 9);
  return IsDigits(id) ? id : std::string();
}

bool EntryMatchesLoaded(const std::string& entry, const std::string& loaded) {
  MapRef r;
  if (loaded.empty() || !ParseEntry(entry, &r)) return false;
  const std::string base = Lower(LoadedBaseName(loaded));
  if (!r.workshop_id.empty()) {
    const std::string lid = WorkshopIdOfLoaded(loaded);
    if (!lid.empty()) return lid == r.workshop_id;
    std::string name = r.name;
    if (name.empty()) name = BoundName(r.workshop_id);
    return !name.empty() && Lower(LoadedBaseName(name)) == base;
  }
  return Lower(LoadedBaseName(r.name)) == base;
}

std::string DisplayName(const std::string& entry) {
  MapRef r;
  if (!ParseEntry(entry, &r)) return entry;
  if (!r.name.empty()) return LoadedBaseName(r.name);
  const std::string bound = BoundName(r.workshop_id);
  return bound.empty() ? "workshop/" + r.workshop_id : bound;
}

void NoteWorkshopLoad(const std::string& workshopId) {
  if (!IsDigits(workshopId)) return;
  std::lock_guard<std::mutex> lk(g_mu);
  g_pending = workshopId;
}

void NoteMapLoaded(const std::string& loaded) {
  const std::string base = LoadedBaseName(loaded);
  if (base.empty()) return;
  const std::string lid = WorkshopIdOfLoaded(loaded);
  std::lock_guard<std::mutex> lk(g_mu);
  if (!lid.empty()) g_bound[lid] = base;
  if (!g_pending.empty()) {
    // The map that loaded after a host_workshop_map is that workshop map (an id inside the
    // loaded name says so directly; a different id means the request was overtaken).
    if (lid.empty() || lid == g_pending) g_bound[g_pending] = base;
    g_pending.clear();
  }
  if (g_bound.size() > 256) g_bound.erase(g_bound.begin());
}

std::string BoundName(const std::string& workshopId) {
  std::lock_guard<std::mutex> lk(g_mu);
  const auto it = g_bound.find(workshopId);
  return it == g_bound.end() ? std::string() : it->second;
}

std::string ReloadEntry(const std::string& loaded) {
  const std::string base = LoadedBaseName(loaded);
  std::string id = WorkshopIdOfLoaded(loaded);
  if (id.empty() && !base.empty()) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (const auto& kv : g_bound) {
      if (kv.second == base) id = kv.first;
    }
  }
  const std::string entry = id.empty() ? base : MakeEntry(base, id);
  return ValidEntry(entry) ? entry : std::string();
}

void ResetBindings() {
  std::lock_guard<std::mutex> lk(g_mu);
  g_bound.clear();
  g_pending.clear();
}

}  // namespace readyup::mapnames
