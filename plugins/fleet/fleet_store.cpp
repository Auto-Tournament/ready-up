#include "fleet_store.h"

#include "fleet_json.h"
#include "fleet_proto.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
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

namespace {

// Printable ASCII, no spaces / quotes / backslashes, 16..200 bytes (what the core accepts).
bool ValidStatusToken(const std::string& t) {
  if (t.size() < 16 || t.size() > 200) return false;
  for (unsigned char c : t) {
    if (c <= 0x20 || c >= 0x7f || c == '"' || c == '\\' || c == '\'') return false;
  }
  return true;
}

void FromSettings(const json::Value& st, ServerConfigLocal* out, std::string* ignored) {
  auto skip = [&](const char* f) {
    if (ignored) *ignored += (ignored->empty() ? "" : ", ") + std::string(f);
  };
  if (const json::Value* v = st.Get("offline_pause_minutes"); v && !v->IsNull()) {
    if (v->t == json::Value::T::Int && v->i >= 0 && v->i <= 1440) out->offlinePauseMinutes = static_cast<int>(v->i);
    else skip("offline_pause_minutes");
  }
  if (const json::Value* h = st.Get("status_http"); h && h->IsObj()) {
    if (const json::Value* v = h->Get("token"); v && !v->IsNull()) {
      if (v->IsStr() && (v->s.empty() || ValidStatusToken(v->s))) out->statusToken = v->s;
      else skip("status_http.token");
    }
  }
}

}  // namespace

bool ParseServerConfigLocal(const std::string& payloadJson, ServerConfigLocal* out, std::string* ignored) {
  *out = ServerConfigLocal{};
  if (ignored) ignored->clear();
  json::Value v;
  if (!json::Parse(payloadJson, &v) || !v.IsObj()) return false;
  const json::Value* st = v.Get("settings");
  if (!st || !st->IsObj()) return false;
  FromSettings(*st, out, ignored);
  return true;
}

bool LoadServerConfigLocal(const std::string& path, ServerConfigLocal* out) {
  *out = ServerConfigLocal{};
  std::string s;
  json::Value v;
  if (!ReadFile(path, &s, 64 * 1024) || !json::Parse(s, &v) || !v.IsObj()) return false;
  FromSettings(v, out, nullptr);
  return true;
}

bool SaveServerConfigLocal(const std::string& path, const ServerConfigLocal& c, std::string* err) {
  json::Value v = json::Value::Object();
  if (c.offlinePauseMinutes >= 0) v.Set("offline_pause_minutes", json::Value::Int(c.offlinePauseMinutes));
  if (!c.statusToken.empty()) {
    json::Value h = json::Value::Object();
    h.Set("token", json::Value::Str(c.statusToken));
    v.Set("status_http", std::move(h));
  }
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

namespace {
bool PluginName(const std::string& n) {
  if (n.empty() || n.size() > 32) return false;
  for (char c : n) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
  }
  return true;
}

void SortCap(std::vector<std::string>* v) {
  std::sort(v->begin(), v->end());
  v->erase(std::unique(v->begin(), v->end()), v->end());
  if (v->size() > 64) v->resize(64);
}
}  // namespace

bool ReadPluginsState(const std::string& pluginsDir, PluginsState* out) {
  *out = PluginsState{};
  if (pluginsDir.empty()) return false;
  DIR* d = opendir(pluginsDir.c_str());
  if (!d) return false;
  while (dirent* e = readdir(d)) {
    const std::string f = e->d_name;
    if (f.size() <= 3 || f.compare(f.size() - 3, 3, ".so") != 0) continue;
    const std::string n = f.substr(0, f.size() - 3);
    if (PluginName(n)) out->installed.push_back(n);
  }
  closedir(d);
  std::string text;
  json::Value v;
  if (ReadFile(pluginsDir + "/plugins.json", &text, 64 * 1024) && json::Parse(text, &v) && v.IsObj()) {
    if (const json::Value* dis = v.Get("disabled"); dis && dis->IsArr()) {
      for (const auto& n : dis->a) {
        if (n.IsStr() && PluginName(n.AsStr())) out->disabled.push_back(n.AsStr());
      }
    }
  }
  SortCap(&out->installed);
  SortCap(&out->disabled);
  return true;
}

}  // namespace fleet
