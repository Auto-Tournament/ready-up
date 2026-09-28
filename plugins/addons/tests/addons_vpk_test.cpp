// ctest `addons_vpk`: VPK directory parsing and the extraction filter.
#include "addons_vpk.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
int g_failures = 0;

void Put32(std::string* s, uint32_t v) { s->append(reinterpret_cast<const char*>(&v), 4); }
void Put16(std::string* s, uint16_t v) { s->append(reinterpret_cast<const char*>(&v), 2); }
void Cstr(std::string* s, const std::string& v) {
  s->append(v);
  s->push_back('\0');
}

// v2 _dir.vpk: vmdl_c in archive 0, vmat_c inline after the tree (0x7FFF) with 3 preload bytes.
std::string MakeVpk() {
  std::string tree;
  Cstr(&tree, "vmdl_c");
  Cstr(&tree, "models/midas");
  Cstr(&tree, "gold");
  Put32(&tree, 0);         // crc
  Put16(&tree, 0);         // preload
  Put16(&tree, 0);         // archive 0
  Put32(&tree, 100);       // offset
  Put32(&tree, 5);         // length
  Put16(&tree, 0xFFFF);    // terminator
  Cstr(&tree, "");         // end of names
  Cstr(&tree, "");         // end of paths
  Cstr(&tree, "vmat_c");
  Cstr(&tree, "materials/midas");
  Cstr(&tree, "white");
  Put32(&tree, 0);
  Put16(&tree, 3);
  Put16(&tree, 0x7FFF);
  Put32(&tree, 0);
  Put32(&tree, 2);
  Put16(&tree, 0xFFFF);
  tree += "abc";           // preload bytes
  Cstr(&tree, "");
  Cstr(&tree, "");
  Cstr(&tree, "");         // end of extensions
  std::string v;
  Put32(&v, 0x55AA1234u);
  Put32(&v, 2);
  Put32(&v, static_cast<uint32_t>(tree.size()));
  Put32(&v, 2);            // file data section size
  Put32(&v, 0);
  Put32(&v, 0);
  Put32(&v, 0);
  v += tree;
  v += "de";
  return v;
}
}  // namespace

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

using namespace addons;

int main() {
  const std::string vpk = MakeVpk();
  VpkDir dir;
  std::string err;
  CHECK(ParseVpkDir(vpk, &dir, &err));
  CHECK(dir.entries.size() == 2);
  if (dir.entries.size() == 2) {
    CHECK(dir.entries[0].path == "models/midas/gold.vmdl_c" && dir.entries[0].archive == 0 &&
          dir.entries[0].offset == 100 && dir.entries[0].length == 5 && dir.entries[0].preload.empty());
    CHECK(dir.entries[1].path == "materials/midas/white.vmat_c" && dir.entries[1].archive == 0x7FFF &&
          dir.entries[1].preload == "abc" && dir.entries[1].length == 2);
    CHECK(vpk.substr(dir.dataStart + dir.entries[1].offset, dir.entries[1].length) == "de");
  }
  CHECK(!ParseVpkDir("nope", &dir, &err));
  std::string bad = vpk;
  bad[0] = 0;
  CHECK(!ParseVpkDir(bad, &dir, &err) && err == "not a VPK");
  CHECK(!ParseVpkDir(vpk.substr(0, 40), &dir, &err));  // truncated

  CHECK(ExtractablePath("models/midas/gold.vmdl_c"));
  CHECK(ExtractablePath("materials/midas/white.vmat_c"));
  CHECK(ExtractablePath("sounds/midas/ding.vsnd_c"));
  CHECK(!ExtractablePath("plugins/skins.so"));
  CHECK(!ExtractablePath("bin/linuxsteamrt64/libserver.so"));
  CHECK(!ExtractablePath("models/../plugins/x"));
  CHECK(!ExtractablePath("/etc/passwd"));
  CHECK(!ExtractablePath("cfg/server.cfg"));

  std::printf("addons_vpk_test: %s\n", g_failures ? "FAIL" : "PASS");
  return g_failures ? 1 : 0;
}
