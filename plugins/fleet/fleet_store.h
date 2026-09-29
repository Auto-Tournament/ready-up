// On-disk state of the fleet link, all under the plugin data dir
// (csgo/readyup/plugins/fleet/):
//   install_id         random id written once (§4.1), survives re-enrollment
//   credentials.json   {server_id, token, ws_url, url, install_id, enrolled_at}, mode 0600
//   spool/             outbound stream (fleet_spool.h)
//   server-config.json the server.config fields fleet.so applies itself, mode 0600
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fleet {

struct Credentials {
  std::string serverId;
  std::string token;  // rus_<id>_<secret>: never logged
  std::string wsUrl;
  std::string url;    // platform base URL it was enrolled against
  std::string installId;
  int64_t enrolledAt = 0;

  bool Valid() const { return !serverId.empty() && !token.empty(); }
};

// mkdir -p with `mode` for created directories.
bool MakeDirs(const std::string& path, unsigned mode, std::string* err);
// Writes tmp (with `mode`), fsyncs, renames over `path`.
bool WriteFileAtomic(const std::string& path, const std::string& data, unsigned mode, std::string* err);
bool ReadFile(const std::string& path, std::string* out, size_t maxBytes = 64u << 20);
bool FileExists(const std::string& path);

bool LoadCredentials(const std::string& path, Credentials* out, std::string* err);
bool SaveCredentials(const std::string& path, const Credentials& c, std::string* err);  // 0600

// The server.config fields (FLEET.md §7.5) fleet.so applies itself; the match plugin applies the
// rest. Both are platform layers over readyup.cfg: absent in the latest server.config = the
// readyup.cfg value again.
//   settings.offline_pause_minutes  -> the offline timer (D12), over [fleet] offline_pause_minutes
//   settings.status_http.token      -> the core's status endpoint token (ru_api set_core_setting
//                                      "status_http_token"), over readyup.cfg status_http_token
struct ServerConfigLocal {
  int offlinePauseMinutes = -1;  // -1 = not set (readyup.cfg); 0 = timer off; at most 1440
  std::string statusToken;       // "" = not set; never logged
};
// server.config payload ({rev, settings}) -> the fields above. Out-of-range or wrong-type values
// are left unset (and named in *ignored). False when the payload has no settings object.
bool ParseServerConfigLocal(const std::string& payloadJson, ServerConfigLocal* out, std::string* ignored);
bool LoadServerConfigLocal(const std::string& path, ServerConfigLocal* out);
bool SaveServerConfigLocal(const std::string& path, const ServerConfigLocal& c, std::string* err);  // 0600

// Reads <dir>/install_id or creates it (26-char ULID). Empty on failure.
std::string LoadOrCreateInstallId(const std::string& dir, std::string* err);

// hello.plugins_state: the Ready Up plugins on this server, read from `pluginsDir`
// (csgo/readyup/plugins). installed = its *.so files by name; disabled = plugins.json "disabled",
// what the core's `ru plugin enable|disable` (and cmd plugins.set) remembers across restarts. A
// disabled name need not be installed. Names outside [a-z0-9_-]{1,32} are skipped; both lists
// are sorted and at most 64 long. False when the directory cannot be read.
struct PluginsState {
  std::vector<std::string> installed;
  std::vector<std::string> disabled;
};
bool ReadPluginsState(const std::string& pluginsDir, PluginsState* out);

}  // namespace fleet
