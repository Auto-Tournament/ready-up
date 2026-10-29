// Engine-free screen specs for the clickable HUD (at_ui.xml; hud_plugin.cpp; ctest `hud_screen`).
//
// Callers describe a whole screen as `key=value` lines (readyup.hud.v1 `screen`, hud_iface.h):
//
//   id=pick                      the screen's identity: a new id plays the leave / enter animation,
//                                the same id updates in place (countdowns, ready dots)
//   where=mid                    card (right, above the weapons) | mid (centre of the lower half)
//   color=live                   acc (default) | live | warn | ban | muted | ct | t
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
//   enter=flip (or fx=)          how it enters: open (unfold + flash) | flip (decisions) | alert (red
//                                flash) | slide | slide-left | pop (overshoot) | zoom | fade | rise |
//                                drop | none.
//                                Default: card slide, mini pop, centre: a decision flips, red alerts,
//                                the rest opens. Any fx works on either panel.
//   exit=close                   how it leaves: fade | close | slide | slide-left | shrink | drop |
//                                rise | flip | none. Default follows the entrance (open closes,
//                                slide slides back, pop shrinks, the rest fade).
//   wash=0 pixels=0 blur=0       turn off the colour wash, the breathing pixels, the blur behind
//   cls=edge glass-3 at-top      utilities on the panel (hud/styles/at_ui.css, "Tailwind for CS2")
//   cls.title=text-2xl tc-live   utilities on one part: brand kind title sub hint timer counter
//                                teams confirm lines cmds buttons close flow b0..b7
//   btn=CT|ct||.ct               a button, up to 24 (kMaxButtons): label | tone | state | command [| count]
//                                tone: green ct t warn danger ghost; state: picked | off | pulse | (none)
//                                (pulse: the one button waiting for this player breathes)
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

// Button slots per panel in at_ui.xml (hud/tools/gen_ui.py).
constexpr int kMaxButtons = 24;


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
  std::string fx;    // enter; empty: picked from the screen (RenderScreen)
  std::string exit;  // empty: follows the entrance (ExitOf)
  std::vector<std::pair<std::string, std::string>> utils;  // (part, "" = the panel; utility class)
  bool wash = true, pixels = true, blur = true;
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

// The spec's utilities as (element id, class) for panel `p`: cls= on the panel, cls.<part>= on
// that part. Names are [a-z0-9-] only; unknown parts are dropped.
std::vector<std::pair<std::string, std::string>> Utilities(const Screen& s, const std::string& p);

// The enter class RenderScreen picks ("fx-open" ...) and the exit class to leave with ("out-close" ...).
std::string EnterOf(const Screen& s, const std::string& p);
std::string ExitOf(const Screen& s, const std::string& p);

// Every exit class, to clear them when the next screen comes in.
const std::vector<std::string>& ExitClasses();

}  // namespace hud
