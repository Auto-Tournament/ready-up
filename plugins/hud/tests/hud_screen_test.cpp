// ctest hud_screen: screen specs (hud_screen.h) parse and render to the right slots.
#include "hud_screen.h"

#include <cstdio>
#include <string>

namespace {

int g_fail = 0;
#define CHECK(c)                                                   \
  do {                                                             \
    if (!(c)) {                                                    \
      std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); \
      ++g_fail;                                                    \
    }                                                              \
  } while (0)

const hud::Write* Find(const std::vector<hud::Write>& w, const std::string& panel, const std::string& name) {
  const hud::Write* last = nullptr;
  for (const auto& x : w)
    if (x.panel == panel && x.name == name) last = &x;
  return last;
}
bool On(const std::vector<hud::Write>& w, const std::string& panel, const std::string& cls) {
  const hud::Write* x = Find(w, panel, cls);
  return x && !x->isText && x->on;
}
std::string Text(const std::vector<hud::Write>& w, const std::string& panel) {
  const hud::Write* x = Find(w, panel, "text");
  return x ? x->value : "<none>";
}

void TestParse() {
  const auto s = hud::ParseScreen(
      "id=pick\nwhere=mid\ncolor=live\nkind=You won the knife\ntitle=Which side do you want?\ntimer=20\n"
      "btn=CT|ct||.ct\nbtn=T|t||.t\nbw=m\nmouse=on\nteams=CT|3|5|T|9|4\nunknown=1\n");
  CHECK(s.id == "pick");
  CHECK(s.where == "mid");
  CHECK(s.buttons.size() == 2);
  CHECK(s.buttons[0].cmd == ".ct");
  CHECK(s.buttons[1].tone == "t");
  CHECK(s.mouse == hud::Mouse::On);
  CHECK(s.hasTeams && s.teamReady[0] == 3 && s.teamDots[1] == 4 && s.teamReady[1] == 4);  // clamped
  CHECK(hud::ParseScreen("").empty());
  CHECK(hud::ParseScreen("where=card\r\ntitle=x\r\n").where == "card");
}

void TestRender() {
  const auto s = hud::ParseScreen("title=Pick\ncolor=warn\nbtn=Unpause|warn||.up\nbtn=Wait|ghost|off|\ncols=1\nclose=Close|\n");
  const auto w = hud::RenderScreen(s, "ui_mid");
  CHECK(Text(w, "ui_mid_title") == "Pick");
  CHECK(On(w, "ui_mid", "c-warn"));
  CHECK(!On(w, "ui_mid", "c-live"));
  CHECK(On(w, "ui_mid", "has-buttons"));
  CHECK(On(w, "ui_mid", "cols-1"));
  CHECK(On(w, "ui_mid", "bw-m"));
  CHECK(On(w, "ui_mid_b0", "show"));
  CHECK(On(w, "ui_mid_b0", "t-warn"));
  CHECK(On(w, "ui_mid_b1", "off"));
  CHECK(!On(w, "ui_mid_b1", "t-ghost"));
  CHECK(!On(w, "ui_mid_b2", "show"));
  CHECK(On(w, "ui_mid", "has-close"));
  CHECK(Text(w, "ui_mid_b0_l") == "Unpause");
  CHECK(!On(w, "ui_mid", "has-kind"));
}

void TestChips() {
  const auto s = hud::ParseScreen("title=Vote\nbtn=Yes|danger||.gg\nbtn=No|ghost||\nhint=Press B to click\nchips=1\n");
  const auto w = hud::RenderScreen(s, "ui_card");
  CHECK(!On(w, "ui_card", "has-buttons"));
  CHECK(On(w, "ui_card", "has-cmds"));
  CHECK(Text(w, "ui_card_k0_c") == ".gg");
  CHECK(On(w, "ui_card_k1", "gone"));  // "No" has no command
  CHECK(!On(w, "ui_card", "has-hint"));
}

void TestLinesAndMini() {
  const auto s = hud::ParseScreen("kind=Ready\ntitle=x\nline=Format|Best of 3\nline=Ready|.r\nsize=mini\n");
  const auto w = hud::RenderScreen(s, "ui_card");
  CHECK(On(w, "ui_card", "mini"));
  CHECK(On(w, "ui_card", "has-lines"));
  CHECK(Text(w, "ui_card_n0_k") == "Format");
  CHECK(!On(w, "ui_card_n0_v", "cmd"));
  CHECK(On(w, "ui_card_n1_v", "cmd"));
  CHECK(On(w, "ui_card_n2", "gone"));
}

void TestMotionClasses() {
  // A decision flips in, its waiting button pulses, and 5 s or less on the timer is urgent.
  auto w = hud::RenderScreen(hud::ParseScreen("title=Which side?\ntimer=4\nbtn=CT|ct|pulse|.ct\nbtn=T|t||.t\nmouse=on\n"), "ui_mid");
  CHECK(On(w, "ui_mid", "fx-flip"));
  CHECK(!On(w, "ui_mid", "fx-open"));
  CHECK(On(w, "ui_mid", "urgent"));
  CHECK(On(w, "ui_mid_b0", "pulse"));
  CHECK(!On(w, "ui_mid_b1", "pulse"));
  // A menu (it has a close button) and a countdown open from the centre; red is an alert.
  w = hud::RenderScreen(hud::ParseScreen("title=Help\nbtn=Ready|green||.r\nclose=Close|#close\nmouse=on\ntimer=20\n"), "ui_mid");
  CHECK(On(w, "ui_mid", "fx-open"));
  CHECK(!On(w, "ui_mid", "urgent"));
  w = hud::RenderScreen(hud::ParseScreen("title=Team B is missing\ncolor=ban\n"), "ui_mid");
  CHECK(On(w, "ui_mid", "fx-alert"));
}

void TestUtilities() {
  const auto s = hud::ParseScreen("title=x\nenter=drop\nexit=rise\ncls=edge glass-3 Bad! at-top\ncls.title=text-2xl tc-live\ncls.nope=bold\n");
  CHECK(hud::EnterOf(s, "ui_mid") == "fx-drop");
  CHECK(hud::ExitOf(s, "ui_mid") == "out-rise");
  const auto u = hud::Utilities(s, "ui_mid");
  auto has = [&](const char* el, const char* c) {
    for (const auto& kv : u)
      if (kv.first == el && kv.second == c) return true;
    return false;
  };
  CHECK(has("ui_mid", "edge"));
  CHECK(has("ui_mid", "at-top"));
  CHECK(!has("ui_mid", "Bad!"));
  CHECK(has("ui_mid_title", "text-2xl"));
  CHECK(has("ui_mid_title", "tc-live"));
  CHECK(u.size() == 5);  // the unknown part is dropped
  // Exits follow the entrance by default.
  CHECK(hud::ExitOf(hud::ParseScreen("title=x\n"), "ui_mid") == "out-close");
  CHECK(hud::ExitOf(hud::ParseScreen("title=x\n"), "ui_card") == "out-slide");
}

void TestButtonIds() {
  std::string p;
  int i = 0;
  CHECK(hud::ParseButtonId("ui_mid_b3", &p, &i) && p == "ui_mid" && i == 3);
  CHECK(hud::ParseButtonId("ui_card_close", &p, &i) && p == "ui_card" && i == -1);
  CHECK(hud::ParseButtonId("ui_mid_b23", &p, &i) && i == 23);
  CHECK(!hud::ParseButtonId("ui_mid_b24", &p, &i));
  CHECK(!hud::ParseButtonId("ui_mid_b", &p, &i));
  CHECK(!hud::ParseButtonId("at_btn_ct", &p, &i));
}

}  // namespace

int main() {
  TestParse();
  TestRender();
  TestChips();
  TestLinesAndMini();
  TestMotionClasses();
  TestUtilities();
  TestButtonIds();
  if (g_fail) std::fprintf(stderr, "hud_screen: %d failure(s)\n", g_fail);
  else std::printf("hud_screen: ok\n");
  return g_fail ? 1 : 0;
}
