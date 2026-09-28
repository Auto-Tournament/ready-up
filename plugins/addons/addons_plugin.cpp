// readyup-addons (plugins/addons, addons.so): Steam Workshop addons for the server.
//
// The server downloads the addons listed in cfg/ReadyUp/addons.cfg (`workshop_addons=<id>,...`)
// and the engine mounts them on the next map change, like it mounts a workshop map. Making
// connecting players download them comes next (the connect / signon flow).
//
// Engine access, all verified before use:
//  - Steam Workshop: Steam's flat C API in libsteam_api.so (stable exported names, like the core's
//    steam_ugc.cpp). Nothing a CS2 update can move.
//  - Mounting: the engine only mounts addons while it loads a map, from the addon list of the
//    pending host-state request (calling its MountAddon from a tick crashed the server). This
//    plugin detours CHostStateMgr::SetPendingHostStateRequest (engine-surface entry
//    Engine2_SetPendingHostStateRequest: libengine2 signature + string anchor, prologue checked by
//    readyup_hookcheck on every CS2 build) and appends the installed addons to the request's
//    m_Addons (CUtlString at +0x58; the request is 0x68 bytes and the engine reads m_Desc +0x10,
//    m_ID +0x1c, m_iMode +0x20 there). The list keeps what the request already had first (a
//    workshop map's own addon). Strings use tier0's allocator (MemAlloc_StrDupFunc / FreeFunc), the
//    one CUtlString frees with.
#include "addons_rules.h"
#include "addons_vpk.h"

#include "readyup/plugin_api.h"
#include "readyup/selftest_iface.h"

#include <dlfcn.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <map>
#include <string>
#include <vector>

namespace addons {
namespace {

const ru_api* g_api = nullptr;

// ---- Steam Workshop (flat C API) -------------------------------------------------------------
using UgcAccessorFn = void* (*)();
using GetItemStateFn = uint32_t (*)(void*, uint64_t);
using DownloadItemFn = bool (*)(void*, uint64_t, bool);
using GetItemInstallInfoFn = bool (*)(void*, uint64_t, uint64_t*, char*, uint32_t, uint32_t*);

struct Steam {
  UgcAccessorFn accessor = nullptr;
  GetItemStateFn itemState = nullptr;
  DownloadItemFn download = nullptr;
  GetItemInstallInfoFn installInfo = nullptr;
  std::string status = "not resolved";
} g_steam;

void ResolveSteam() {
  if (g_steam.accessor) return;
  void* lib = dlopen("libsteam_api.so", RTLD_NOW | RTLD_NOLOAD);
  if (!lib) {
    g_steam.status = "libsteam_api.so not loaded";
    return;
  }
  for (int v = 30; v >= 10 && !g_steam.accessor; --v) {
    const std::string name = "SteamAPI_SteamGameServerUGC_v0" + std::to_string(v);
    g_steam.accessor = reinterpret_cast<UgcAccessorFn>(dlsym(lib, name.c_str()));
    if (g_steam.accessor) g_steam.status = name;
  }
  g_steam.itemState = reinterpret_cast<GetItemStateFn>(dlsym(lib, "SteamAPI_ISteamUGC_GetItemState"));
  g_steam.download = reinterpret_cast<DownloadItemFn>(dlsym(lib, "SteamAPI_ISteamUGC_DownloadItem"));
  g_steam.installInfo = reinterpret_cast<GetItemInstallInfoFn>(dlsym(lib, "SteamAPI_ISteamUGC_GetItemInstallInfo"));
  if (!g_steam.accessor) g_steam.status = "no SteamAPI_SteamGameServerUGC_v0xx export";
  else if (!g_steam.itemState || !g_steam.download || !g_steam.installInfo) g_steam.status = "missing ISteamUGC exports";
}

void* Ugc() {
  ResolveSteam();
  if (!g_steam.accessor || !g_steam.itemState || !g_steam.download || !g_steam.installInfo) return nullptr;
  return g_steam.accessor();  // NULL until the game server is logged on to Steam
}

// ---- tier0 allocator (CUtlString's) ------------------------------------------------------------
using StrDupFn = char* (*)(const char*);
using FreeFn = void (*)(void*);
StrDupFn g_strdup = nullptr;
FreeFn g_free = nullptr;

bool ResolveTier0() {
  if (g_strdup && g_free) return true;
  void* t0 = dlopen("libtier0.so", RTLD_NOW | RTLD_NOLOAD);
  if (!t0) return false;
  g_strdup = reinterpret_cast<StrDupFn>(dlsym(t0, "MemAlloc_StrDupFunc"));
  g_free = reinterpret_cast<FreeFn>(dlsym(t0, "MemAlloc_FreeFunc"));
  return g_strdup && g_free;
}

// ---- State -------------------------------------------------------------------------------------
struct Item {
  bool downloadAsked = false;
  double askedAt = 0;  // when the download was requested (retried after 30 s without progress)
  bool installed = false;
  bool mounted = false;  // attached to a map change (the engine logs "Mounting addon '<id>'")
  bool refused = false;
  bool updateChecked = false;  // DownloadItem asked for an installed item (Steam fetches a newer version)
  uint32_t lastState = 0xFFFFFFFFu;
};
std::vector<uint64_t> g_ids;  // from addons.cfg, in order
std::map<uint64_t, Item> g_items;
double g_lastConfig = -1e9, g_lastPoll = -1e9;
std::string g_hookStatus = "not installed";

std::string ConfigValue(const char* key) {
  char buf[1024] = {};
  return g_api->config_get(g_api->self, key, buf, sizeof(buf)) > 0 ? std::string(buf) : std::string();
}

// ---- The map-change hook -----------------------------------------------------------------------
constexpr int kRequestAddons = 0x58;  // CHostStateRequest::m_Addons (CUtlString)
using SetPendingFn = void (*)(void*, void*);
SetPendingFn g_origSetPending = nullptr;

// "a,b" + installed ids not in it yet -> "a,b,c". Empty when nothing to add.
std::string MergedAddons(const char* current) {
  std::string out = current ? current : "";
  int bad = 0;
  const auto have = ParseIds(out, &bad);
  bool added = false;
  for (uint64_t id : g_ids) {
    const auto it = g_items.find(id);
    if (it == g_items.end() || !it->second.installed) continue;
    if (std::find(have.begin(), have.end(), id) != have.end()) continue;
    out += (out.empty() ? "" : ",") + std::to_string(id);
    added = true;
  }
  return added ? out : std::string();
}

void DetourSetPending(void* mgr, void* request) {
  if (g_api && request && ResolveTier0()) {
    char** addons = reinterpret_cast<char**>(static_cast<unsigned char*>(request) + kRequestAddons);
    const std::string merged = MergedAddons(*addons);
    if (!merged.empty()) {
      char* dup = g_strdup(merged.c_str());
      if (dup) {
        if (*addons) g_free(*addons);
        *addons = dup;
        for (auto& kv : g_items) {
          if (kv.second.installed) kv.second.mounted = true;
          kv.second.updateChecked = false;  // check for a newer version again
        }
        ru_logf(g_api, RU_LOG_INFO, "map change: addons %s", merged.c_str());
      }
    }
  }
  g_origSetPending(mgr, request);
}

void InstallHook() {
  void* tramp = nullptr;
  if (g_api->hook_function(g_api->self, "Engine2_SetPendingHostStateRequest", reinterpret_cast<void*>(&DetourSetPending),
                           &tramp) == 1) {
    g_origSetPending = reinterpret_cast<SetPendingFn>(tramp);
    g_hookStatus = "ok";
  } else {
    g_hookStatus = "refused (see log): addons are downloaded but not mounted";
  }
}

// ---- Precache: the map's resource manifest ------------------------------------------------------
// Detour of CGameRulesGameSystem::OnBuildGameSessionManifest (IGameSystem slot 7; engine-surface
// entry CGameRulesGameSystem_BuildGameSessionManifest): after the game's own resources, add the
// `precache=` paths to msg->m_pResourceManifest. AddResource(const char*) is CEntityResourceManifest
// slot 0, a thunk `xor r8d,r8d; xor r9d,r9d; xor ecx,ecx; xor edx,edx; jmp <impl>`: checked before
// every use, so a changed engine skips the precache instead of calling something else.
using ManifestEventFn = void (*)(void*, const void*);
using AddResourceFn = void (*)(void*, const char*);
ManifestEventFn g_origManifest = nullptr;
std::string g_precacheStatus = "not installed";
std::vector<std::string> g_precache;  // `precache=` from addons.cfg

bool IsAddResourceThunk(const void* fn) {
  static const uint8_t kThunk[] = {0x45, 0x31, 0xC0, 0x45, 0x31, 0xC9, 0x31, 0xC9, 0x31, 0xD2, 0xE9};
  return fn && std::memcmp(fn, kThunk, sizeof(kThunk)) == 0;
}

void DetourManifest(void* self, const void* msg) {
  g_origManifest(self, msg);
  if (!g_api || !msg || g_precache.empty()) return;
  void* manifest = *static_cast<void* const*>(msg);
  if (!manifest) return;
  void** vt = *static_cast<void***>(manifest);
  if (!vt || !IsAddResourceThunk(vt[0])) {
    g_precacheStatus = "skipped: IEntityResourceManifest slot 0 is not AddResource on this build";
    ru_logf(g_api, RU_LOG_WARN, "precache %s", g_precacheStatus.c_str());
    return;
  }
  auto add = reinterpret_cast<AddResourceFn>(vt[0]);
  for (const std::string& path : g_precache) add(manifest, path.c_str());
  g_precacheStatus = "ok (" + std::to_string(g_precache.size()) + " resource(s) with the last map load)";
  ru_logf(g_api, RU_LOG_INFO, "precache: added %zu resource(s) to the map's manifest", g_precache.size());
}

void InstallPrecacheHook() {
  void* tramp = nullptr;
  if (g_api->hook_function(g_api->self, "CGameRulesGameSystem_BuildGameSessionManifest",
                           reinterpret_cast<void*>(&DetourManifest), &tramp) == 1) {
    g_origManifest = reinterpret_cast<ManifestEventFn>(tramp);
    g_precacheStatus = "ok (waiting for a map load)";
  } else {
    g_precacheStatus = "refused (see log): addon models will show as ERROR";
  }
}

std::vector<std::string> SplitList(const std::string& text) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : text + ",") {
    if (c == ',' || c == ';') {
      while (!cur.empty() && (cur.back() == ' ' || cur.back() == '\t')) cur.pop_back();
      size_t i = cur.find_first_not_of(" \t");
      if (i != std::string::npos) out.push_back(cur.substr(i));
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  return out;
}

// ---- Extraction: server-side content ------------------------------------------------------------
// The engine's addon mount (the map-change request) makes clients load an addon but does not put
// its files in the server's GAME search path, so precaching its models failed ("File not found").
// Ready Up's own `Game csgo/readyup` line in gameinfo.gi is a GAME search path: the addon's content
// files (models/, materials/, ...) are copied there as loose files once per addon version (the
// install timestamp, kept in plugins/addons/extracted_<id>.txt with the file list).
std::string g_root;  // csgo/readyup

std::string ReadAll(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return f ? ss.str() : std::string();
}

bool MakeDirs(const std::string& path) {
  for (size_t i = 1; i <= path.size(); ++i) {
    if (i == path.size() || path[i] == '/') {
      const std::string d = path.substr(0, i);
      if (mkdir(d.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
  }
  return true;
}

// True when the addon's files are in place (extracted now or already for this version).
bool ExtractAddon(void* ugc, uint64_t id) {
  char folder[1024] = {};
  uint64_t size = 0;
  uint32_t stamp = 0;
  if (!g_steam.installInfo(ugc, id, &size, folder, sizeof(folder), &stamp)) {  // stamp: logged only
    ru_logf(g_api, RU_LOG_WARN, "addon %llu: no install info from Steam", static_cast<unsigned long long>(id));
    return false;
  }
  const std::string base = std::string(folder) + "/" + std::to_string(id);
  // The version on disk: size + mtime of the archives. Steam's install timestamp can be newer than
  // the files while an update is still being fetched, so it doesn't say which files are there.
  std::string version;
  for (const char* suffix : {"_dir.vpk", ".vpk", "_000.vpk"}) {
    struct stat st {};
    if (stat((base + suffix).c_str(), &st) == 0) {
      version += std::string(suffix) + ":" + std::to_string(st.st_size) + ":" + std::to_string(st.st_mtime) + " ";
    }
  }
  const std::string marker = g_root + "/plugins/addons/extracted_" + std::to_string(id) + ".txt";
  const std::string wantHead = "files " + version + "\n";
  if (ReadAll(marker).compare(0, wantHead.size(), wantHead) == 0) return true;  // these files are out already

  std::string dirBytes = ReadAll(base + "_dir.vpk");
  if (dirBytes.empty()) dirBytes = ReadAll(base + ".vpk");  // single-file (older) addons
  VpkDir dir;
  std::string err;
  if (!ParseVpkDir(dirBytes, &dir, &err)) {
    ru_logf(g_api, RU_LOG_WARN, "addon %llu: can't read its VPK (%s)", static_cast<unsigned long long>(id), err.c_str());
    return false;
  }
  std::string list = wantHead;
  int files = 0;
  for (const VpkEntry& e : dir.entries) {
    if (!ExtractablePath(e.path)) continue;
    std::string data = e.preload;
    if (e.length) {
      if (e.archive == 0x7FFF) {
        if (static_cast<size_t>(dir.dataStart) + e.offset + e.length > dirBytes.size()) continue;
        data += dirBytes.substr(dir.dataStart + e.offset, e.length);
      } else {
        char n[16];
        std::snprintf(n, sizeof(n), "_%03u.vpk", static_cast<unsigned>(e.archive));
        std::ifstream a(base + n, std::ios::binary);
        std::string chunk(e.length, '\0');
        if (!a.seekg(e.offset) || !a.read(&chunk[0], e.length)) continue;
        data += chunk;
      }
    }
    const std::string dst = g_root + "/" + e.path;
    if (!MakeDirs(dst.substr(0, dst.rfind('/')))) continue;
    std::ofstream o(dst, std::ios::binary | std::ios::trunc);
    o.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!o) continue;
    list += e.path + "\n";
    ++files;
  }
  std::ofstream(marker, std::ios::trunc) << list;
  ru_logf(g_api, RU_LOG_INFO, "addon %llu: %d content file(s) into csgo/readyup for the server (steam version %u)",
          static_cast<unsigned long long>(id), files, stamp);
  return true;
}

// ---- Polling -----------------------------------------------------------------------------------
void Poll(double now) {
  void* ugc = Ugc();
  if (!ugc) return;  // not logged on yet: next poll
  for (uint64_t id : g_ids) {
    Item& it = g_items[id];
    const uint32_t st = g_steam.itemState(ugc, id);
    if (st != it.lastState) {
      // Installed again (a first install or an update): extract again; a no-op for the same version.
      if ((st & kItemInstalled) && !(st & (kItemDownloading | kItemDownloadPending | kItemNeedsUpdate))) {
        it.installed = false;
        it.downloadAsked = false;
      }
      it.lastState = st;
      ru_logf(g_api, RU_LOG_INFO, "addon %llu: %s", static_cast<unsigned long long>(id), ItemStateText(st).c_str());
    }
    // A freshly published item can take a while before Steam serves it: ask again when nothing happened.
    if (it.downloadAsked && !(st & (kItemInstalled | kItemDownloading | kItemDownloadPending)) && now - it.askedAt > 30.0) {
      it.downloadAsked = false;
    }
    switch (NextAction(st, it.mounted, it.downloadAsked)) {
      case Action::kDownload:
        it.downloadAsked = g_steam.download(ugc, id, true);
        it.askedAt = now;
        ru_logf(g_api, it.downloadAsked ? RU_LOG_INFO : RU_LOG_WARN, "addon %llu: download %s",
                static_cast<unsigned long long>(id), it.downloadAsked ? "requested" : "refused by Steam");
        break;
      case Action::kMount:
        if (!it.installed && ExtractAddon(ugc, id)) {
          it.installed = true;
          ru_logf(g_api, RU_LOG_INFO, "addon %llu: installed; mounts with the next map change (.ru map)",
                  static_cast<unsigned long long>(id));
        }
        break;
      case Action::kRefuse:
        if (!it.refused) ru_logf(g_api, RU_LOG_WARN, "addon %llu: legacy item, not a CS2 addon", static_cast<unsigned long long>(id));
        it.refused = true;
        break;
      case Action::kNone:
        break;
    }
    // New files on disk (an update that finished after the state change): extract them. Cheap when
    // nothing changed (a few stat() calls and one small file read).
    if (it.installed && (st & kItemInstalled) && !(st & (kItemDownloading | kItemDownloadPending))) ExtractAddon(ugc, id);
    // A dedicated server's Steam does not refresh "needs update" on its own: ask once per plugin load
    // and map change; Steam downloads only if a newer version is published.
    if ((st & kItemInstalled) && !(st & (kItemDownloading | kItemDownloadPending)) && !it.updateChecked) {
      it.updateChecked = true;
      g_steam.download(ugc, id, true);
    }
  }
}

void OnTick(void*, const ru_tick_info* t) {
  if (t->now - g_lastConfig >= 5.0) {
    g_lastConfig = t->now;
    int bad = 0;
    auto ids = ParseIds(ConfigValue("workshop_addons"), &bad);
    if (ids != g_ids) {
      g_ids = std::move(ids);
      ru_logf(g_api, RU_LOG_INFO, "workshop_addons: %zu addon(s)%s", g_ids.size(), bad ? " (some entries are not workshop ids)" : "");
    }
    auto pre = SplitList(ConfigValue("precache"));
    if (pre != g_precache) {
      g_precache = std::move(pre);
      ru_logf(g_api, RU_LOG_INFO, "precache: %zu resource(s) (applied with the next map load)", g_precache.size());
    }
  }
  if (t->now - g_lastPoll >= 2.0 && !g_ids.empty()) {
    g_lastPoll = t->now;
    Poll(t->now);
  }
}

std::string StatusLine(uint64_t id) {
  const Item& it = g_items[id];
  std::string s = std::to_string(id) + ": " + (it.lastState == 0xFFFFFFFFu ? "unknown" : ItemStateText(it.lastState));
  if (it.mounted) s += ", mounted with this map";
  else if (it.installed) s += ", mounts with the next map change";
  return s;
}

void Reply(const ru_command_ctx* ctx, const std::string& msg) {
  const std::string line = "Ready Up addons: " + msg;
  if (ctx->slot >= 0) g_api->chat_to_slot(g_api->self, ctx->slot, line.c_str());
  else ru_logf(g_api, RU_LOG_INFO, "%s", msg.c_str());
}

// `ru addons`: status of every configured addon.
void OnRu(void*, const ru_command_ctx* ctx) {
  ResolveSteam();
  Reply(ctx, std::to_string(g_ids.size()) + " addon(s); steam " + g_steam.status + "; map-change hook " + g_hookStatus +
                 "; precache " + g_precacheStatus);
  for (uint64_t id : g_ids) Reply(ctx, StatusLine(id));
}

void RunSelftest(ru_selftest_add_fn add, void* ctx) {
  ResolveSteam();
  add(ctx, g_hookStatus == "ok" ? "INFO" : "WARN", "addons",
      (std::to_string(g_ids.size()) + " addon(s), steam " + g_steam.status + ", map-change hook " + g_hookStatus +
       ", precache " + g_precacheStatus).c_str());
  for (uint64_t id : g_ids) add(ctx, "INFO", "addons", StatusLine(id).c_str());
}
const ru_selftest_iface_v1 g_selftestIface = {sizeof(ru_selftest_iface_v1), &RunSelftest};

}  // namespace
}  // namespace addons

using namespace addons;

extern "C" {

READYUP_PLUGIN_EXPORT const ru_plugin_info* readyup_plugin_info(void) {
  static const ru_plugin_info info = {
      sizeof(ru_plugin_info),
      (1u << 16) | 8u,  // needs API 1.8 (hook_function)
      "addons",
      ADDONS_VERSION,
      "Ready Up",
      "Steam Workshop addons for the server (cfg/ReadyUp/addons.cfg)",
  };
  return &info;
}

READYUP_PLUGIN_EXPORT int readyup_plugin_load(const ru_api* api, uint32_t core_api_version) {
  if (RU_API_VERSION_MAJOR(core_api_version) != 1 || !RU_API_HAS(api, hook_function)) return 1;
  g_api = api;
  g_ids.clear();
  g_items.clear();
  g_lastConfig = g_lastPoll = -1e9;
  const char* dd = api->data_dir(api->self);  // csgo/readyup/plugins/addons
  g_root = dd ? std::string(dd) : std::string();
  while (!g_root.empty() && g_root.back() == '/') g_root.pop_back();
  for (int up = 0; up < 2 && g_root.rfind('/') != std::string::npos; ++up) g_root.erase(g_root.rfind('/'));
  g_origSetPending = nullptr;
  g_origManifest = nullptr;
  g_precache.clear();
  InstallHook();
  InstallPrecacheHook();
  api->on_tick(api->self, &OnTick, nullptr);
  api->register_ru_subcommand(api->self, "addons", &OnRu, nullptr);
  if (RU_API_HAS(api, provide_interface)) {
    api->provide_interface(api->self, RU_SELFTEST_IFACE_PREFIX "addons", RU_SELFTEST_IFACE_VERSION,
                           const_cast<ru_selftest_iface_v1*>(&g_selftestIface));
  }
  ru_logf(api, RU_LOG_INFO, "loaded " ADDONS_VERSION " (workshop_addons in cfg/ReadyUp/addons.cfg; map-change hook %s)",
          g_hookStatus.c_str());
  return 0;
}

READYUP_PLUGIN_EXPORT void readyup_plugin_unload(void) {
  // The core removes the detour after this returns. Mounted addons stay until the next map change.
  ru_logf(g_api, RU_LOG_INFO, "unloaded");
  g_api = nullptr;
}

}  // extern "C"
