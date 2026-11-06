// ctest `hud_addon`: hud/styles/at_ui.css knows every addon version up to hud::kAddonVersion and
// no newer one, so the "HUD out of date" notice hides on the addon built from this tree. Fix a
// failure with `python hud/tools/gen_ui.py`.
#include "hud_addon.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

int main() {
  std::ifstream in(AT_UI_CSS);
  if (!in) {
    std::printf("FAIL: cannot read %s\n", AT_UI_CSS);
    return 1;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string css = ss.str();
  int failed = 0;
  for (int n = 1; n <= hud::kAddonVersion + 1; ++n) {
    const std::string rule = ".ui-ver.check.v-" + std::to_string(n) + " { visibility: collapse; }";
    const bool has = css.find(rule) != std::string::npos;
    const bool want = n <= hud::kAddonVersion;
    if (has != want) {
      std::printf("FAIL: at_ui.css %s \"%s\" (kAddonVersion %d); run python hud/tools/gen_ui.py\n",
                  want ? "lacks" : "has", rule.c_str(), hud::kAddonVersion);
      ++failed;
    }
  }
  if (!failed) std::printf("ok: at_ui.css knows addon versions 1..%d\n", hud::kAddonVersion);
  return failed ? 1 : 0;
}
