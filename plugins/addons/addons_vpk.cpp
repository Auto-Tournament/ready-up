#include "addons_vpk.h"

#include <cstring>

namespace addons {
namespace {

template <typename T>
bool Rd(const std::string& b, size_t at, T* v) {
  if (at + sizeof(T) > b.size()) return false;
  std::memcpy(v, b.data() + at, sizeof(T));
  return true;
}

bool Str(const std::string& b, size_t* at, std::string* out) {
  const size_t end = b.find('\0', *at);
  if (end == std::string::npos) return false;
  *out = b.substr(*at, end - *at);
  *at = end + 1;
  return true;
}

}  // namespace

bool ParseVpkDir(const std::string& b, VpkDir* out, std::string* error) {
  auto fail = [&](const char* why) {
    if (error) *error = why;
    return false;
  };
  uint32_t sig = 0, ver = 0, tree = 0;
  if (!Rd(b, 0, &sig) || !Rd(b, 4, &ver) || !Rd(b, 8, &tree)) return fail("too short");
  if (sig != 0x55AA1234u) return fail("not a VPK");
  if (ver != 1 && ver != 2) return fail("unknown VPK version");
  const size_t header = ver == 2 ? 28 : 12;
  if (header + tree > b.size()) return fail("truncated tree");
  out->entries.clear();
  out->dataStart = static_cast<uint32_t>(header + tree);
  size_t at = header;
  const size_t treeEnd = header + tree;
  for (;;) {
    std::string ext;
    if (!Str(b, &at, &ext) || at > treeEnd) return fail("bad extension");
    if (ext.empty()) break;
    for (;;) {
      std::string dir;
      if (!Str(b, &at, &dir) || at > treeEnd) return fail("bad path");
      if (dir.empty()) break;
      for (;;) {
        std::string name;
        if (!Str(b, &at, &name) || at > treeEnd) return fail("bad name");
        if (name.empty()) break;
        uint32_t crc = 0, offset = 0, length = 0;
        uint16_t preload = 0, archive = 0, term = 0;
        if (!Rd(b, at, &crc) || !Rd(b, at + 4, &preload) || !Rd(b, at + 6, &archive) || !Rd(b, at + 8, &offset) ||
            !Rd(b, at + 12, &length) || !Rd(b, at + 16, &term)) {
          return fail("truncated entry");
        }
        at += 18;
        if (term != 0xFFFF || at + preload > treeEnd) return fail("bad entry");
        VpkEntry e;
        e.path = (dir == " " ? std::string() : dir + "/") + name + "." + ext;
        e.preload = b.substr(at, preload);
        e.archive = archive;
        e.offset = offset;
        e.length = length;
        at += preload;
        out->entries.push_back(std::move(e));
      }
    }
  }
  return true;
}

bool ExtractablePath(const std::string& p) {
  if (p.empty() || p[0] == '/' || p.find("..") != std::string::npos || p.find('\\') != std::string::npos) return false;
  static const char* const kRoots[] = {"models/", "materials/", "sounds/", "soundevents/", "particles/", "panorama/images/"};
  for (const char* r : kRoots) {
    if (p.compare(0, std::strlen(r), r) == 0) return true;
  }
  return false;
}

}  // namespace addons
