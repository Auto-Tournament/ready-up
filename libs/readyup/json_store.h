#pragma once
// Small JSON files instead of a database (docs/FLEET.md D13: Postgres is gone from Ready Up).
//
// Every file is one object with a schema version, e.g. {"version": 1, "admins": [...]}:
//   - Load() reads it. A file that is unreadable, not JSON, not an object, or has a missing or
//     newer "version" is renamed to <path>.corrupt-<unix time> and reported as Corrupt, so the
//     caller starts fresh (and logs `note`) instead of refusing to run or overwriting it blind.
//   - Save() writes <path>.tmp.<pid>, fsyncs it, renames it over <path> and fsyncs the
//     directory: readers only ever see the old or the new file, never half of one.
//   - FileLock is an advisory flock(2) on <path>.lock for writers in other processes
//     (scripts/migrate-postgres-to-json.py, scripts/seed-dev-skins.py take the same lock).
// Inside a plugin exactly one thread writes each file (the callers' writer threads).
// Engine-free, no core dependencies: plugins compile this file directly.
#include "readyup/status_snapshot.h"

#include <cstdint>
#include <string>

namespace readyup::json_store {

using Json = status::Json;

enum class LoadResult { Ok, Missing, Corrupt };

// `maxVersion`: the newest schema this build understands.
LoadResult Load(const std::string& path, int maxVersion, Json* out, std::string* note);

// Adds "version" first when `doc` has none. Pretty-printed (2 spaces), trailing newline.
bool Save(const std::string& path, const Json& doc, int version, std::string* err);

// mkdir -p (0755).
bool MakeDirs(const std::string& dir, std::string* err);

// 0 when missing. Nanosecond mtime, for "reload when the file changed on disk".
int64_t MtimeNs(const std::string& path);

std::string Pretty(const Json& v);

class FileLock {
 public:
  explicit FileLock(const std::string& path);  // blocks until locked (best-effort: never throws)
  ~FileLock();
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;

 private:
  int fd_ = -1;
};

}  // namespace readyup::json_store
