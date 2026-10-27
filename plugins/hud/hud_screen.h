// Engine-free screen specs for the clickable HUD (at_ui.xml; hud_plugin.cpp; ctest `hud_screen`).
//
// Callers describe a whole screen as `key=value` lines (readyup.hud.v1 `screen`, hud_iface.h):
//
//   id=pick                      the screen's identity: a new id plays the leave / enter animation,
//                                the same id updates in place (countdowns, ready dots)
//   where=mid                    card (right, above the weapons) | mid (centre of the lower half)
//   color=live                   acc (default) | live | warn | ban | muted
//   kind=You won the knife       small coloured label above the title
//   title=Which side do you want?
//   sub=Match starts (5)         grey line under the title
//   timer=20                     top right
//   counter=1 / 3 used           bottom right
//   hint=Press B to click        small grey line at the bottom
//   teams=CT|3|5|T|2|5           two teams: name | ready | dots (0-5)
//   confirm=Team A|1|Team B|0    two chips: name | READY (1) or WAITING (0)
//   cmd=Timeout|.tac             a typed command chip, up to 3
//   line=Format|Best of 3        an info row, up to 5: label | value (a value starting with . is boxed)
//   size=mini                    card only: a one-line pill (kind, team dots, the first button)
//   btn=CT|ct||.ct               a button, up to 8: label | tone | state | command [| count]
//                                tone: green ct t warn danger ghost; state: picked | off | (none)
//   cols=2                       buttons per row (default: the number of buttons, at most 4)
//   bw=m                         button width: s m l xl (default m)
//   close=Close|                 a close / back button: label | command (empty = just hide)
//   mouse=on                     off (default) | b (cursor while the buy menu is open) | on
//   chips=1                      mid-round: buttons are drawn as typed commands, never a cursor
//
// Unknown keys are ignored, so callers can add keys before the plugin knows them.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace hud {

enum class Mouse { Off, B, On };

struct Button {
  std::string label, tone, state, cmd, sub;
};

struct Screen {
  std::string id, where = "mid", color, kind, title, sub, timer, counter, hint;
  bool hasTeams = false;
  std::string teamName[2];
  int teamReady[2] = {0, 0}, teamDots[2] = {5, 5};
  bool hasConfirm = false;
  std::string confirmName[2];
  bool confirmReady[2] = {false, false};
  std::vector<std::pair<std::string, std::string>> cmds;  // label, command
  std::vector<std::pair<std::string, std::string>> lines;  // label, value
  bool mini = false;
  std::vector<Button> buttons;
  int cols = 0;
  std::string bw = "m";
  bool hasClose = false;
  std::string closeLabel, closeCmd;
  Mouse mouse = Mouse::Off;
  bool chips = false;

  bool empty() const { return title.empty() && kind.empty(); }
};

Screen ParseScreen(const std::string& spec);

// One write onto the layout: a dialog variable "text" (isText) or a class on a panel.
struct Write {
  std::string panel, name, value;
  bool isText = false;
  bool on = false;
};

// Every text and class of panel `p` ("ui_card" or "ui_mid") for screen `s`, except "on". Writes
// the full set each time (unused slots off), so a caller that dedupes only sends what changed.
std::vector<Write> RenderScreen(const Screen& s, const std::string& p);

// The panel a button id belongs to and the button's index: "ui_mid_b3" -> ("ui_mid", 3); the close
// button -> index -1. False when the id is not one of ours.
bool ParseButtonId(const std::string& id, std::string* panel, int* index);

}  // namespace hud
