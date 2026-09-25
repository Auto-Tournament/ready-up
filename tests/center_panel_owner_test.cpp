// ctest `center_panel_owner`: one center panel per player, by priority (core/src/readyup/center_panel_owner.h).
#include "readyup/center_panel_owner.h"

#include <cstdio>

namespace {
int g_failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      ++g_failures;                                                    \
    }                                                                  \
  } while (0)
}  // namespace

int main() {
  using readyup::CenterPanelOwners;
  constexpr uint64_t kMatch = 1, kEssentials = 2, kPractice = 3;
  constexpr int kHud = 50, kAlert = 90;
  CenterPanelOwners o;

  // The ready HUD draws, then the download bar (higher) takes the panel over.
  CHECK(o.Claim(3, kMatch, kHud, 2, 0.0));
  CHECK(o.Claim(3, kEssentials, kAlert, 1, 0.5));
  CHECK(o.Owner(3, 0.6) == kEssentials);
  // While the bar is up the HUD is refused, on that slot only.
  CHECK(!o.Claim(3, kMatch, kHud, 2, 1.0));
  CHECK(o.Claim(4, kMatch, kHud, 2, 1.0));
  // The owner refreshes itself freely; the bar stops, so its panel expires and the HUD is back.
  CHECK(o.Claim(3, kEssentials, kAlert, 1, 1.2));
  CHECK(!o.Claim(3, kMatch, kHud, 2, 2.1));
  CHECK(o.Claim(3, kMatch, kHud, 2, 2.3));
  CHECK(o.Owner(3, 2.4) == kMatch);
  // Equal priority: the newer send wins (as before there was an owner).
  CHECK(o.Claim(3, kPractice, kHud, 2, 2.5));
  // Release hands the panel back at once; Reset (disconnect, map change) clears it.
  CHECK(o.Claim(5, kEssentials, kAlert, 5, 3.0));
  CHECK(!o.Claim(5, kMatch, kHud, 2, 3.1));
  o.Release(-1, kEssentials);
  CHECK(o.Claim(5, kMatch, kHud, 2, 3.2));
  CHECK(o.Claim(6, kEssentials, kAlert, 5, 3.0));
  o.Reset(6);
  CHECK(o.Owner(6, 3.1) == 0 && o.Claim(6, kMatch, kHud, 2, 3.1));
  // Out of range.
  CHECK(!o.Claim(-1, kMatch, kHud, 1, 0) && !o.Claim(64, kMatch, kHud, 1, 0) && o.Owner(99, 0) == 0);

  std::printf("center_panel_owner_test: %s\n", g_failures ? "FAIL" : "PASS");
  return g_failures ? 1 : 0;
}
