// On-disk state of the fleet link, all under the plugin data dir
// (csgo/readyup/plugins/fleet/):
//   install_id         random id written once (§4.1), survives re-enrollment
//   credentials.json   {server_id, token, ws_url, url, install_id, enrolled_at}, mode 0600
//   spool/             outbound stream (fleet_spool.h)
#pragma once

#include <cstdint>
#include <string>

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

// Reads <dir>/install_id or creates it (26-char ULID). Empty on failure.
std::string LoadOrCreateInstallId(const std::string& dir, std::string* err);

}  // namespace fleet
