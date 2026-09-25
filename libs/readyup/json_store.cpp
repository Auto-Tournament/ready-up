#include "readyup/json_store.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace readyup::json_store {
namespace {

bool ReadAll(const std::string& path, std::string* out, bool* missing) {
  *missing = false;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    *missing = errno == ENOENT;
    return false;
  }
  out->clear();
  char buf[16384];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
    out->append(buf, n);
    if (out->size() > (64u << 20)) {  // nothing of ours is that big
      std::fclose(f);
      return false;
    }
  }
  const bool ok = !std::ferror(f);
  std::fclose(f);
  return ok;
}

std::string DirOf(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? std::string(".") : (slash == 0 ? std::string("/") : path.substr(0, slash));
}

void PrettyTo(std::string& out, const Json& v, int depth) {
  const auto indent = [&](int d) { out.append(static_cast<size_t>(d) * 2, ' '); };
  if (v.type() == Json::Type::Object) {
    if (v.Members().empty()) {
      out += "{}";
      return;
    }
    out += "{\n";
    bool first = true;
    for (const auto& kv : v.Members()) {
      if (!first) out += ",\n";
      first = false;
      indent(depth + 1);
      status::JsonEscapeTo(out, kv.first);  // adds the quotes
      out += ": ";
      PrettyTo(out, kv.second, depth + 1);
    }
    out += '\n';
    indent(depth);
    out += '}';
    return;
  }
  if (v.type() == Json::Type::Array) {
    if (v.Items().empty()) {
      out += "[]";
      return;
    }
    // Arrays of scalars / small objects (table rows) stay one line per item.
    out += "[\n";
    bool first = true;
    for (const auto& item : v.Items()) {
      if (!first) out += ",\n";
      first = false;
      indent(depth + 1);
      item.DumpTo(out);
    }
    out += '\n';
    indent(depth);
    out += ']';
    return;
  }
  v.DumpTo(out);
}

}  // namespace

std::string Pretty(const Json& v) {
  std::string out;
  PrettyTo(out, v, 0);
  out += '\n';
  return out;
}

LoadResult Load(const std::string& path, int maxVersion, Json* out, std::string* note) {
  std::string text;
  bool missing = false;
  std::string why;
  if (!ReadAll(path, &text, &missing)) {
    if (missing) return LoadResult::Missing;
    why = std::string("unreadable (") + std::strerror(errno) + ")";
  } else {
    Json doc;
    std::string perr;
    if (!Json::Parse(text, &doc, &perr)) {
      why = "not valid JSON (" + perr + ")";
    } else if (!doc.IsObject()) {
      why = "not a JSON object";
    } else {
      const Json* ver = doc.Find("version");
      if (!ver || ver->type() != Json::Type::Int || ver->AsInt() < 1) {
        why = "no \"version\"";
      } else if (ver->AsInt() > maxVersion) {
        why = "version " + std::to_string(ver->AsInt()) + " is newer than this build understands (" +
              std::to_string(maxVersion) + ")";
      } else {
        *out = std::move(doc);
        return LoadResult::Ok;
      }
    }
  }
  const std::string backup = path + ".corrupt-" + std::to_string(static_cast<long long>(std::time(nullptr)));
  if (std::rename(path.c_str(), backup.c_str()) == 0) {
    if (note) *note = path + ": " + why + "; moved to " + backup + ", starting fresh";
  } else {
    if (note) *note = path + ": " + why + "; could not move it aside (" + std::strerror(errno) + "), starting fresh";
  }
  return LoadResult::Corrupt;
}

bool MakeDirs(const std::string& dir, std::string* err) {
  if (dir.empty()) return true;
  std::string cur;
  size_t i = 0;
  while (i <= dir.size()) {
    const size_t slash = dir.find('/', i);
    cur = dir.substr(0, slash == std::string::npos ? dir.size() : slash);
    i = slash == std::string::npos ? dir.size() + 1 : slash + 1;
    if (cur.empty()) continue;
    if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) {
      if (err) *err = "mkdir " + cur + ": " + std::strerror(errno);
      return false;
    }
  }
  return true;
}

bool Save(const std::string& path, const Json& doc, int version, std::string* err) {
  Json out = Json::Object();
  out["version"] = version;
  for (const auto& kv : doc.Members()) {
    if (kv.first != "version") out[kv.first] = kv.second;
  }
  const std::string data = Pretty(out);
  const std::string dir = DirOf(path);
  if (!MakeDirs(dir, err)) return false;
  static unsigned s_counter = 0;
  const std::string tmp = path + ".tmp." + std::to_string(::getpid()) + "." + std::to_string(++s_counter);
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) {
    if (err) *err = "open " + tmp + ": " + std::strerror(errno);
    return false;
  }
  size_t off = 0;
  while (off < data.size()) {
    const ssize_t w = ::write(fd, data.data() + off, data.size() - off);
    if (w < 0) {
      if (errno == EINTR) continue;
      if (err) *err = "write " + tmp + ": " + std::strerror(errno);
      ::close(fd);
      ::unlink(tmp.c_str());
      return false;
    }
    off += static_cast<size_t>(w);
  }
  if (::fsync(fd) != 0 || ::close(fd) != 0) {
    if (err) *err = "fsync " + tmp + ": " + std::strerror(errno);
    ::unlink(tmp.c_str());
    return false;
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    if (err) *err = "rename " + tmp + ": " + std::strerror(errno);
    ::unlink(tmp.c_str());
    return false;
  }
  const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dfd >= 0) {
    (void)::fsync(dfd);
    ::close(dfd);
  }
  return true;
}

int64_t MtimeNs(const std::string& path) {
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) return 0;
  return static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec + st.st_size;
}

FileLock::FileLock(const std::string& path) {
  std::string err;
  (void)MakeDirs(DirOf(path), &err);
  fd_ = ::open((path + ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd_ < 0) return;
  while (::flock(fd_, LOCK_EX) != 0 && errno == EINTR) {
  }
}

FileLock::~FileLock() {
  if (fd_ >= 0) {
    (void)::flock(fd_, LOCK_UN);
    ::close(fd_);
  }
}

}  // namespace readyup::json_store
