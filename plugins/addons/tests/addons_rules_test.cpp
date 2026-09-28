// ctest `addons_rules`: workshop id list parsing and the per-item action.
#include "addons_rules.h"

#include <cstdio>

namespace {
int g_failures = 0;
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
  int bad = -1;
  CHECK(ParseIds("", &bad).empty() && bad == 0);
  auto ids = ParseIds("3808995594, 123 3808995594;x9,0,  456", &bad);
  CHECK(ids.size() == 3 && ids[0] == 3808995594ull && ids[1] == 123 && ids[2] == 456);
  CHECK(bad == 2);  // "x9" and "0"
  CHECK(ParseIds("99999999999999999999999", &bad).empty() && bad == 1);  // too long

  CHECK(ItemStateText(0) == "not installed");
  CHECK(ItemStateText(kItemInstalled) == "installed");
  CHECK(ItemStateText(kItemInstalled | kItemNeedsUpdate) == "installed, update available");
  CHECK(ItemStateText(kItemDownloading | kItemInstalled) == "downloading");
  CHECK(ItemStateText(kItemLegacy | kItemInstalled) == "legacy (not a Source 2 addon)");

  CHECK(NextAction(0, false, false) == Action::kDownload);
  CHECK(NextAction(0, false, true) == Action::kNone);  // already asked
  CHECK(NextAction(kItemDownloading, false, false) == Action::kNone);
  CHECK(NextAction(kItemInstalled, false, true) == Action::kMount);
  CHECK(NextAction(kItemInstalled | kItemNeedsUpdate, false, false) == Action::kDownload);  // update
  CHECK(NextAction(kItemInstalled | kItemNeedsUpdate, false, true) == Action::kMount);  // asked already
  CHECK(NextAction(kItemInstalled, true, false) == Action::kNone);
  CHECK(NextAction(kItemLegacy | kItemInstalled, false, false) == Action::kRefuse);

  std::printf("addons_rules_test: %s\n", g_failures ? "FAIL" : "PASS");
  return g_failures ? 1 : 0;
}
