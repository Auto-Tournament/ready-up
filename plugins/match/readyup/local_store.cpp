#include "readyup/local_store.h"

#include "readyup/json_store.h"
#include "readyup/logging.h"
#include "readyup/workers.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <map>
#include <mutex>
#include <utility>

namespace readyup::local_store {
namespace {

using json_store::Json;

constexpr int kStateVersion = 1;
constexpr int kAdminsVersion = 1;
constexpr int kFleetAdminsVersion = 1;
constexpr const char* kStateFile = "state.json";
constexpr const char* kAdminsFile = "admins.json";
constexpr const char* kFleetAdminsFile = "fleet-admins.json";

enum Dirty : unsigned { kDirtyState = 1u, kDirtyAdmins = 2u, kDirtyFleet = 4u };

std::mutex g_mu;  // everything below
std::condition_variable g_cv;
std::string g_dir;
std::map<std::string, std::string> g_settings;
std::vector<Admin> g_admins;
int64_t g_adminsMtime = 0;
std::vector<Admin> g_fleetAdmins;
int64_t g_fleetRev = -1;  // -1: never received
unsigned g_dirty = 0;
bool g_writerRunning = false;
std::atomic<bool> g_fleetMode{false};

uint64_t SteamOf(const Json* v) {
  if (!v) return 0;
  if (v->type() == Json::Type::String) return std::strtoull(v->AsString().c_str(), nullptr, 10);
  if (v->type() == Json::Type::Int && v->AsInt() > 0) return static_cast<uint64_t>(v->AsInt());
  return 0;
}

std::vector<Admin> AdminsOf(const Json& doc) {
  std::vector<Admin> out;
  const Json* arr = doc.Find("admins");
  if (!arr || arr->type() != Json::Type::Array) return out;
  for (const Json& a : arr->Items()) {
    Admin e;
    if (a.IsObject()) {
      e.steamid64 = SteamOf(a.Find("steamid64"));
      if (const Json* n = a.Find("name"); n && n->type() == Json::Type::String) e.name = n->AsString();
    } else {
      e.steamid64 = SteamOf(&a);  // plain SteamID64 list is fine too
    }
    if (e.steamid64 == 0) continue;
    if (std::none_of(out.begin(), out.end(), [&](const Admin& x) { return x.steamid64 == e.steamid64; })) {
      out.push_back(std::move(e));
    }
  }
  return out;
}

Json AdminsJson(const std::vector<Admin>& admins) {
  Json arr = Json::Array();
  for (const auto& a : admins) {
    Json o = Json::Object();
    o["steamid64"] = std::to_string(a.steamid64);
    o["name"] = a.name;
    arr.Push(std::move(o));
  }
  return arr;
}

std::string PathLocked(const char* file) { return g_dir + "/" + file; }

bool LoadDoc(const std::string& path, int version, Json* doc) {
  std::string note;
  switch (json_store::Load(path, version, doc, &note)) {
    case json_store::LoadResult::Ok:
      return true;
    case json_store::LoadResult::Corrupt:
      Print("store: %s\n", note.c_str());
      return false;
    case json_store::LoadResult::Missing:
      return false;
  }
  return false;
}

// Writer thread (or the caller when no thread can be started, e.g. during unload).
void Flush() {
  unsigned dirty;
  std::string dir;
  Json state = Json::Object(), admins = Json::Object(), fleet = Json::Object();
  {
    std::lock_guard<std::mutex> lk(g_mu);
    dirty = g_dirty;
    g_dirty = 0;
    dir = g_dir;
    if (dirty & kDirtyState) {
      Json s = Json::Object();
      for (const auto& kv : g_settings) s[kv.first] = kv.second;
      state["settings"] = std::move(s);
    }
    if (dirty & kDirtyAdmins) admins["admins"] = AdminsJson(g_admins);
    if (dirty & kDirtyFleet) {
      fleet["rev"] = static_cast<long long>(g_fleetRev);
      fleet["admins"] = AdminsJson(g_fleetAdmins);
    }
  }
  if (dir.empty() || !dirty) return;
  const auto save = [&](const char* file, const Json& doc, int version) {
    const std::string path = dir + "/" + file;
    json_store::FileLock lock(path);
    std::string err;
    if (!json_store::Save(path, doc, version, &err)) Print("store: saving %s failed: %s\n", path.c_str(), err.c_str());
    return path;
  };
  if (dirty & kDirtyState) save(kStateFile, state, kStateVersion);
  if (dirty & kDirtyAdmins) {
    const std::string path = save(kAdminsFile, admins, kAdminsVersion);
    std::lock_guard<std::mutex> lk(g_mu);
    g_adminsMtime = json_store::MtimeNs(path);
  }
  if (dirty & kDirtyFleet) save(kFleetAdminsFile, fleet, kFleetAdminsVersion);
}

void WriterMain() {
  for (;;) {
    {
      std::unique_lock<std::mutex> lk(g_mu);
      g_cv.wait(lk, [] { return g_dirty != 0 || workers::ShuttingDown(); });
      if (g_dirty == 0) {  // shutting down, nothing pending
        g_writerRunning = false;
        return;
      }
    }
    Flush();
  }
}

void MarkDirty(unsigned bits) {
  std::unique_lock<std::mutex> lk(g_mu);
  g_dirty |= bits;
  if (!g_writerRunning) {
    static bool s_wakerAdded = false;
    if (!s_wakerAdded) {
      s_wakerAdded = true;
      workers::AddWaker([] {
        std::lock_guard<std::mutex> l(g_mu);
        g_cv.notify_all();
      });
    }
    g_writerRunning = workers::Spawn("store-writer", WriterMain);
    if (!g_writerRunning) {  // unloading: save on this thread
      lk.unlock();
      Flush();
      return;
    }
  }
  lk.unlock();
  g_cv.notify_one();
}

}  // namespace

void Init(const std::string& dataDir) {
  Json state, admins, fleet;
  const bool haveState = LoadDoc(dataDir + "/" + kStateFile, kStateVersion, &state);
  const bool haveAdmins = LoadDoc(dataDir + "/" + kAdminsFile, kAdminsVersion, &admins);
  const bool haveFleet = LoadDoc(dataDir + "/" + kFleetAdminsFile, kFleetAdminsVersion, &fleet);
  std::lock_guard<std::mutex> lk(g_mu);
  g_dir = dataDir;
  g_settings.clear();
  if (haveState) {
    if (const Json* s = state.Find("settings"); s && s->IsObject()) {
      for (const auto& kv : s->Members()) {
        if (kv.second.type() == Json::Type::String && !kv.second.AsString().empty()) {
          g_settings[kv.first] = kv.second.AsString();
        }
      }
    }
  }
  g_admins = haveAdmins ? AdminsOf(admins) : std::vector<Admin>{};
  g_adminsMtime = json_store::MtimeNs(PathLocked(kAdminsFile));
  g_fleetAdmins = haveFleet ? AdminsOf(fleet) : std::vector<Admin>{};
  g_fleetRev = -1;
  if (haveFleet) {
    if (const Json* r = fleet.Find("rev"); r && r->type() == Json::Type::Int) g_fleetRev = r->AsInt();
  }
}

std::string Path(const char* file) {
  std::lock_guard<std::mutex> lk(g_mu);
  return PathLocked(file);
}

void SetSetting(const std::string& key, std::optional<std::string> value) {
  if (key.empty()) return;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (value && !value->empty()) {
      auto it = g_settings.find(key);
      if (it != g_settings.end() && it->second == *value) return;
      g_settings[key] = std::move(*value);
    } else if (g_settings.erase(key) == 0) {
      return;
    }
  }
  MarkDirty(kDirtyState);
}

std::optional<std::string> GetSetting(const std::string& key) {
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_settings.find(key);
  if (it == g_settings.end() || it->second.empty()) return std::nullopt;
  return it->second;
}

std::vector<Admin> LocalAdmins() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_admins;
}

bool AddLocalAdmin(uint64_t steamid64, const std::string& name) {
  if (steamid64 == 0) return false;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    for (const auto& a : g_admins) {
      if (a.steamid64 == steamid64) return false;
    }
    g_admins.push_back(Admin{steamid64, name});
  }
  MarkDirty(kDirtyAdmins);
  return true;
}

bool RemoveLocalAdmin(uint64_t steamid64) {
  {
    std::lock_guard<std::mutex> lk(g_mu);
    const auto it = std::remove_if(g_admins.begin(), g_admins.end(),
                                   [&](const Admin& a) { return a.steamid64 == steamid64; });
    if (it == g_admins.end()) return false;
    g_admins.erase(it, g_admins.end());
  }
  MarkDirty(kDirtyAdmins);
  return true;
}

bool IsLocalAdmin(uint64_t steamid64) {
  std::lock_guard<std::mutex> lk(g_mu);
  return std::any_of(g_admins.begin(), g_admins.end(), [&](const Admin& a) { return a.steamid64 == steamid64; });
}

void ReloadAdminsIfChanged() {
  std::string path;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_dir.empty() || (g_dirty & kDirtyAdmins)) return;  // our own change is not saved yet
    path = PathLocked(kAdminsFile);
    if (json_store::MtimeNs(path) == g_adminsMtime) return;
  }
  Json doc;
  const bool ok = LoadDoc(path, kAdminsVersion, &doc);
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_dirty & kDirtyAdmins) return;
  g_admins = ok ? AdminsOf(doc) : std::vector<Admin>{};
  g_adminsMtime = json_store::MtimeNs(path);
  Print("store: reloaded %s (%zu admin(s))\n", path.c_str(), g_admins.size());
}

void SetFleetMode(bool on) { g_fleetMode.store(on); }
bool FleetMode() { return g_fleetMode.load(); }

bool SetFleetAdmins(int64_t rev, std::vector<Admin> admins) {
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (rev < g_fleetRev) return false;
    g_fleetRev = rev;
    g_fleetAdmins = std::move(admins);
  }
  MarkDirty(kDirtyFleet);
  return true;
}

std::vector<Admin> FleetAdmins(int64_t* rev) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (rev) *rev = g_fleetRev;
  return g_fleetAdmins;
}

bool IsFleetAdmin(uint64_t steamid64) {
  std::lock_guard<std::mutex> lk(g_mu);
  return std::any_of(g_fleetAdmins.begin(), g_fleetAdmins.end(),
                     [&](const Admin& a) { return a.steamid64 == steamid64; });
}

std::string Summary() {
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_dir.empty()) return "not initialized";
  std::string s = "JSON in " + g_dir + ": " + std::to_string(g_settings.size()) + " setting(s), ";
  if (g_fleetMode.load()) {
    s += "admins from the platform (" + std::to_string(g_fleetAdmins.size()) + ", rev " +
         (g_fleetRev < 0 ? std::string("none yet") : std::to_string(g_fleetRev)) + ")";
  } else {
    s += std::to_string(g_admins.size()) + " admin(s) in admins.json";
  }
  return s;
}

}  // namespace readyup::local_store
