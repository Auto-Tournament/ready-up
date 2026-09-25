#include "fleet_store.h"

#include "fleet_json.h"
#include "fleet_proto.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace fleet {

namespace {
std::string Errno(const std::string& what) { return what + ": " + std::strerror(errno); }
}  // namespace

bool MakeDirs(const std::string& path, unsigned mode, std::string* err) {
  if (path.empty()) return true;
  std::string cur;
  size_t i = 0;
  while (i <= path.size()) {
    const size_t j = path.find('/', i);
    const size_t end = j == std::string::npos ? path.size() : j;
    cur = path.substr(0, end);
    if (!cur.empty()) {
      if (mkdir(cur.c_str(), mode) != 0 && errno != EEXIST) {
        if (err) *err = Errno("mkdir " + cur);
        return false;
      }
    }
    if (j == std::string::npos) break;
    i = j + 1;
  }
  struct stat st {};
  if (stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
    if (err) *err = "not a directory: " + path;
    return false;
  }
  return true;
}

bool WriteFileAtomic(const std::string& path, const std::string& data, unsigned mode, std::string* err) {
  const std::string tmp = path + ".tmp." + std::to_string(getpid());
  const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
  if (fd < 0) {
    if (err) *err = Errno("open " + tmp);
    return false;
  }
  fchmod(fd, mode);  // umask must not widen or narrow it
  size_t off = 0;
  while (off < data.size()) {
    const ssize_t w = write(fd, data.data() + off, data.size() - off);
    if (w < 0) {
      if (errno == EINTR) continue;
      if (err) *err = Errno("write " + tmp);
      close(fd);
      unlink(tmp.c_str());
      return false;
    }
    off += static_cast<size_t>(w);
  }
  fsync(fd);
  close(fd);
  if (rename(tmp.c_str(), path.c_str()) != 0) {
    if (err) *err = Errno("rename " + tmp);
    unlink(tmp.c_str());
    return false;
  }
  return true;
}

bool ReadFile(const std::string& path, std::string* out, size_t maxBytes) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::string s;
  char buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
    s.append(buf, n);
    if (s.size() > maxBytes) {
      std::fclose(f);
      return false;
    }
  }
  std::fclose(f);
  *out = std::move(s);
  return true;
}

bool FileExists(const std::string& path) {
  struct stat st {};
  return stat(path.c_str(), &st) == 0;
}

bool LoadCredentials(const std::string& path, Credentials* out, std::string* err) {
  std::string text;
  if (!ReadFile(path, &text, 1u << 16)) {
    if (err) *err = FileExists(path) ? "unreadable" : "missing";
    return false;
  }
  json::Value v;
  std::string perr;
  if (!json::Parse(text, &v, &perr) || !v.IsObj()) {
    if (err) *err = "corrupt credentials file: " + perr;
    return false;
  }
  Credentials c;
  c.serverId = v.Get("server_id") ? v.Get("server_id")->AsStr() : "";
  c.token = v.Get("token") ? v.Get("token")->AsStr() : "";
  c.wsUrl = v.Get("ws_url") ? v.Get("ws_url")->AsStr() : "";
  c.url = v.Get("url") ? v.Get("url")->AsStr() : "";
  c.installId = v.Get("install_id") ? v.Get("install_id")->AsStr() : "";
  c.enrolledAt = v.Get("enrolled_at") ? v.Get("enrolled_at")->AsInt(0) : 0;
  if (!c.Valid()) {
    if (err) *err = "credentials file lacks server_id/token";
    return false;
  }
  // Tighten a file someone copied in with wider permissions.
  struct stat st {};
  if (stat(path.c_str(), &st) == 0 && (st.st_mode & 077) != 0) chmod(path.c_str(), 0600);
  *out = std::move(c);
  return true;
}

bool SaveCredentials(const std::string& path, const Credentials& c, std::string* err) {
  json::Value v = json::Value::Object();
  v.Set("server_id", json::Value::Str(c.serverId));
  v.Set("token", json::Value::Str(c.token));
  v.Set("ws_url", json::Value::Str(c.wsUrl));
  v.Set("url", json::Value::Str(c.url));
  v.Set("install_id", json::Value::Str(c.installId));
  v.Set("enrolled_at", json::Value::Int(c.enrolledAt));
  return WriteFileAtomic(path, json::Dump(v) + "\n", 0600, err);
}

std::string LoadOrCreateInstallId(const std::string& dir, std::string* err) {
  const std::string path = dir + "/install_id";
  std::string s;
  if (ReadFile(path, &s, 256)) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    if (s.size() >= 16 && s.size() <= 64) return s;
  }
  const std::string id = NewUlid(NowMs());
  if (!WriteFileAtomic(path, id + "\n", 0644, err)) return {};
  return id;
}

}  // namespace fleet
