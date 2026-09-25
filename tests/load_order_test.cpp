// Offline tests for running next to Metamod (docs/COMPATIBILITY.md): path canonicalization of
// the loader's module names, telling Valve's server library from Metamod's, the gameinfo.gi
// order check and the one-shim-per-process guard.
//   cmake --build build && (cd build && ctest --output-on-failure)

#include "readyup/load_order.h"
#include "readyup/path.h"

#include <cstdio>
#include <string>

using namespace readyup;

static int g_failures = 0;

#define CHECK(cond)                                                                  \
  do {                                                                               \
    if (!(cond)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                  \
    }                                                                                \
  } while (0)

static void CheckStr(const std::string& a, const std::string& b, const char* expr, int line) {
  if (a == b) return;
  std::fprintf(stderr, "%s:%d: CHECK_STR failed: %s: \"%s\" != \"%s\"\n", __FILE__, line, expr, a.c_str(),
               b.c_str());
  ++g_failures;
}
#define CHECK_STR(a, b) CheckStr((a), (b), #a, __LINE__)

static const char* kRoot = "/home/cs2/readyup-test/game";

static void TestPaths() {
  // What the loader reports for our shim when Metamod loads it (seen on a live server).
  const std::string viaMetamod =
      std::string(kRoot) + "/bin/linuxsteamrt64/../../csgo/readyup/bin/linuxsteamrt64//libserver.so";
  CHECK_STR(CanonicalizePath(viaMetamod), std::string(kRoot) + "/csgo/readyup/bin/linuxsteamrt64/libserver.so");
  CHECK_STR(CanonicalizePath("/a/./b//c/"), "/a/b/c");
  CHECK_STR(CanonicalizePath("a/../../b"), "../b");
  CHECK_STR(CanonicalizePath("/.."), "/");

  // <csgo> from the module dir, including the trailing-slash spelling that used to yield
  // <csgo>/readyup (status.json ended up in csgo/readyup/readyup/).
  const std::string csgo = std::string(kRoot) + "/csgo";
  CHECK_STR(CsgoDirFromModuleDir(csgo + "/readyup/bin/linuxsteamrt64"), csgo);
  CHECK_STR(CsgoDirFromModuleDir(std::string(kRoot) + "/bin/linuxsteamrt64/../../csgo/readyup/bin/linuxsteamrt64/"),
            csgo);
  CHECK_STR(CsgoDirFromModuleDir(""), "");
  CHECK_STR(CsgoDirFromModuleDir("/a/b"), "");
}

static void TestValveModule() {
  const std::string csgo = std::string(kRoot) + "/csgo";
  CHECK(LooksLikeValveServerModule(csgo + "/bin/linuxsteamrt64/libserver.so"));
  CHECK(LooksLikeValveServerModule(csgo + "/readyup/bin/linuxsteamrt64/../../../bin/linuxsteamrt64/libserver.so"));
  // Metamod's proxy is loaded first under Metamod; scanning it made every signature miss.
  CHECK(!LooksLikeValveServerModule(csgo + "/addons/metamod/bin/linuxsteamrt64/libserver.so"));
  CHECK(!LooksLikeValveServerModule(csgo + "/readyup/bin/linuxsteamrt64/libserver.so"));
  CHECK(!LooksLikeValveServerModule(std::string(kRoot) +
                                    "/bin/linuxsteamrt64/../../csgo/readyup/bin/linuxsteamrt64//libserver.so"));
  CHECK(!LooksLikeValveServerModule(csgo + "/bin/linuxsteamrt64/libserver_valve.so"));
  CHECK(!LooksLikeValveServerModule(""));
}

static std::string Gameinfo(const std::string& lines) {
  return "\"GameInfo\"\n{\n\tFileSystem\n\t{\n\t\tSearchPaths\n\t\t{\n"
         "\t\t\tGame_LowViolence\tcsgo_lv // Perfect World content override\n" +
         lines +
         "\t\t\tGame\tcsgo_imported\n\t\t\tGame\tcsgo_core\n\t\t\tGame\tcore\n"
         "\t\t\tMod\t\tcsgo\n\t\t\tAddonRoot\t\t\tcsgo_addons\n\t\t}\n\t}\n}\n";
}

static void TestLoadOrder() {
  const std::string ru = "\t\t\tGame\tcsgo/readyup\r\n";
  const std::string mm = "\t\t\tGame    csgo/addons/metamod // added by metamod\n";
  const std::string cs = "\t\t\tGame\tcsgo\n";

  auto p = ParseGameSearchPaths(Gameinfo(ru + cs));
  CHECK(p.readyup == 0 && p.csgo == 1 && p.metamod == -1 && p.readyup_count == 1);
  CHECK_STR(DescribeLoadOrderProblem(p), "");

  // Supported: Metamod, then Ready Up, then csgo.
  p = ParseGameSearchPaths(Gameinfo(mm + ru + cs));
  CHECK(p.metamod == 0 && p.readyup == 1 && p.csgo == 2);
  CHECK_STR(DescribeLoadOrderProblem(p), "");

  // Wrong order: Metamod never loads.
  p = ParseGameSearchPaths(Gameinfo(ru + mm + cs));
  CHECK(p.readyup == 0 && p.metamod == 1);
  CHECK(DescribeLoadOrderProblem(p).find("BELOW csgo/readyup") != std::string::npos);

  // Metamod below `Game csgo` never takes effect: nothing to warn about.
  p = ParseGameSearchPaths(Gameinfo(ru + cs + mm));
  CHECK_STR(DescribeLoadOrderProblem(p), "");

  // Two Ready Up lines.
  p = ParseGameSearchPaths(Gameinfo(ru + mm + ru + cs));
  CHECK(p.readyup_count == 2);
  CHECK(DescribeLoadOrderProblem(p).find("listed 2 times") != std::string::npos);

  // Not listed in this file (e.g. only in gameinfo_branchspecific.gi): cannot judge.
  p = ParseGameSearchPaths(Gameinfo(mm + cs));
  CHECK(p.readyup == -1);
  CHECK_STR(DescribeLoadOrderProblem(p), "");

  // Only the SearchPaths block counts; commented-out lines are ignored.
  p = ParseGameSearchPaths("Game csgo/readyup\n" + Gameinfo("\t\t\t// Game csgo/addons/metamod\n" + ru + cs));
  CHECK(p.readyup == 0 && p.metamod == -1);
}

static void TestInstanceGuard() {
  std::string other;
  CHECK(ShimInstanceMayRun("", 100, "/x/readyup/libserver.so", &other));
  CHECK(ShimInstanceMayRun("100 /x/readyup/libserver.so", 100, "/x/readyup/libserver.so", &other));
  // Marker inherited from a parent process: not ours to honour.
  CHECK(ShimInstanceMayRun("99 /y/readyup/libserver.so", 100, "/x/readyup/libserver.so", &other));
  CHECK(ShimInstanceMayRun("garbage", 100, "/x/readyup/libserver.so", &other));
  // A second copy in the same process stays inert.
  other.clear();
  CHECK(!ShimInstanceMayRun("100 /y/readyup/libserver.so", 100, "/x/readyup/libserver.so", &other));
  CHECK_STR(other, "/y/readyup/libserver.so");
}

int main() {
  TestPaths();
  TestValveModule();
  TestLoadOrder();
  TestInstanceGuard();
  if (g_failures) {
    std::fprintf(stderr, "load_order_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("load_order_test: OK\n");
  return 0;
}
