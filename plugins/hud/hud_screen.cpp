#include "hud_screen.h"

#include <algorithm>
#include <cstdlib>

namespace hud {
namespace {

std::vector<std::string> SplitBar(const std::string& s) {
  std::vector<std::string> out;
  size_t start = 0;
  for (;;) {
    const size_t bar = s.find('|', start);
    out.push_back(s.substr(start, bar == std::string::npos ? std::string::npos : bar - start));
    if (bar == std::string::npos) break;
    start = bar + 1;
  }
  return out;
}

std::string At(const std::vector<std::string>& v, size_t i) { return i < v.size() ? v[i] : std::string(); }
int IntAt(const std::vector<std::string>& v, size_t i, int def) {
  return i < v.size() && !v[i].empty() ? std::atoi(v[i].c_str()) : def;
}

const char* const kColours[] = {"c-live", "c-warn", "c-ban", "c-muted"};
const char* const kTones[] = {"t-green", "t-ct", "t-t", "t-warn", "t-danger"};
const char* const kWidths[] = {"bw-s", "bw-m", "bw-l", "bw-xl"};

}  // namespace

Screen ParseScreen(const std::string& spec) {
  Screen s;
  size_t start = 0;
  while (start < spec.size()) {
    size_t nl = spec.find('\n', start);
    if (nl == std::string::npos) nl = spec.size();
    std::string line = spec.substr(start, nl - start);
    start = nl + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    if (k == "id") s.id = v;
    else if (k == "where") s.where = v == "card" ? "card" : "mid";
    else if (k == "color") s.color = v;
    else if (k == "kind") s.kind = v;
    else if (k == "title") s.title = v;
    else if (k == "sub") s.sub = v;
    else if (k == "timer") s.timer = v;
    else if (k == "counter") s.counter = v;
    else if (k == "hint") s.hint = v;
    else if (k == "teams") {
      const auto p = SplitBar(v);
      s.hasTeams = true;
      for (int t = 0; t < 2; ++t) {
        s.teamName[t] = At(p, t * 3);
        s.teamDots[t] = std::clamp(IntAt(p, t * 3 + 2, 5), 0, 5);
        s.teamReady[t] = std::clamp(IntAt(p, t * 3 + 1, 0), 0, s.teamDots[t]);
      }
    } else if (k == "confirm") {
      const auto p = SplitBar(v);
      s.hasConfirm = true;
      for (int t = 0; t < 2; ++t) {
        s.confirmName[t] = At(p, t * 2);
        s.confirmReady[t] = IntAt(p, t * 2 + 1, 0) != 0;
      }
    } else if (k == "cmd") {
      const auto p = SplitBar(v);
      if (s.cmds.size() < 3) s.cmds.emplace_back(At(p, 0), At(p, 1));
    } else if (k == "btn") {
      const auto p = SplitBar(v);
      if (s.buttons.size() < 8) s.buttons.push_back(Button{At(p, 0), At(p, 1), At(p, 2), At(p, 3), At(p, 4)});
    } else if (k == "cols") {
      s.cols = std::clamp(std::atoi(v.c_str()), 1, 4);
    } else if (k == "bw") {
      if (v == "s" || v == "m" || v == "l" || v == "xl") s.bw = v;
    } else if (k == "close") {
      const auto p = SplitBar(v);
      s.hasClose = !At(p, 0).empty();
      s.closeLabel = At(p, 0);
      s.closeCmd = At(p, 1);
    } else if (k == "mouse") {
      s.mouse = v == "on" ? Mouse::On : v == "b" ? Mouse::B : Mouse::Off;
    } else if (k == "chips") {
      s.chips = v == "1";
    }
  }
  return s;
}

std::vector<Write> RenderScreen(const Screen& s, const std::string& p) {
  std::vector<Write> w;
  auto text = [&](const std::string& panel, const std::string& v) { w.push_back(Write{panel, "text", v, true, false}); };
  auto cls = [&](const std::string& panel, const std::string& c, bool on) { w.push_back(Write{panel, c, "", false, on}); };

  const std::string colour = s.color.empty() || s.color == "acc" ? "" : "c-" + s.color;
  for (const char* c : kColours) cls(p, c, colour == c);

  text(p + "_kind", s.kind);
  text(p + "_title", s.title);
  text(p + "_sub", s.sub);
  text(p + "_timer", s.timer);
  text(p + "_counter", s.counter);
  text(p + "_hint", s.chips ? std::string() : s.hint);
  cls(p, "has-kind", !s.kind.empty());
  cls(p, "has-sub", !s.sub.empty());
  cls(p, "has-timer", !s.timer.empty());
  cls(p, "has-counter", !s.counter.empty());
  cls(p, "has-hint", !s.chips && !s.hint.empty());

  cls(p, "has-teams", s.hasTeams);
  for (int t = 0; t < 2; ++t) {
    const std::string side = t == 0 ? "A" : "B";
    text(p + "_t" + side, s.teamName[t]);
    for (int n = 0; n < 5; ++n) {
      const std::string d = p + "_d" + side + std::to_string(n);
      cls(d, "on", s.hasTeams && n < s.teamReady[t]);
      cls(d, "gone", n >= s.teamDots[t]);
    }
  }

  cls(p, "has-confirm", s.hasConfirm);
  for (int t = 0; t < 2; ++t) {
    const std::string c = p + "_c" + std::to_string(t);
    text(c + "_team", s.confirmName[t]);
    text(c + "_state", s.confirmReady[t] ? "READY" : "WAITING");
    cls(c, "ready", s.confirmReady[t]);
  }

  // Mid-round the cursor never comes out: the buttons that run a typed command become chips.
  std::vector<std::pair<std::string, std::string>> cmds = s.cmds;
  if (s.chips) {
    for (const Button& b : s.buttons)
      if (cmds.size() < 3 && !b.cmd.empty() && b.cmd[0] == '.' && b.state != "off") cmds.emplace_back(b.label, b.cmd);
  }
  cls(p, "has-cmds", !cmds.empty());
  for (size_t i = 0; i < 3; ++i) {
    const std::string k = p + "_k" + std::to_string(i);
    text(k + "_l", i < cmds.size() ? cmds[i].first : std::string());
    text(k + "_c", i < cmds.size() ? cmds[i].second : std::string());
    cls(k, "gone", i >= cmds.size());
  }

  const size_t nb = s.chips ? 0 : s.buttons.size();
  cls(p, "has-buttons", nb > 0);
  const int cols = s.cols > 0 ? s.cols : static_cast<int>(std::clamp<size_t>(nb, 1, 4));
  for (int c = 1; c <= 4; ++c) cls(p, "cols-" + std::to_string(c), c == cols);
  for (const char* bw : kWidths) cls(p, bw, std::string("bw-") + s.bw == bw);
  for (size_t i = 0; i < 8; ++i) {
    const std::string b = p + "_b" + std::to_string(i);
    const Button* btn = i < nb ? &s.buttons[i] : nullptr;
    text(b + "_l", btn ? btn->label : std::string());
    text(b + "_s", btn ? btn->sub : std::string());
    cls(b, "show", btn != nullptr);
    cls(b, "has-sub", btn && !btn->sub.empty());
    const std::string tone = btn ? "t-" + btn->tone : std::string();
    for (const char* t : kTones) cls(b, t, btn && btn->state != "off" && tone == t);
    cls(b, "picked", btn && btn->state == "picked");
    cls(b, "off", btn && btn->state == "off");
  }

  cls(p, "has-close", !s.chips && s.hasClose);
  text(p + "_close_l", s.closeLabel);
  return w;
}

bool ParseButtonId(const std::string& id, std::string* panel, int* index) {
  for (const char* p : {"ui_card", "ui_mid"}) {
    const std::string pre = std::string(p) + "_";
    if (id.compare(0, pre.size(), pre) != 0) continue;
    const std::string rest = id.substr(pre.size());
    if (rest == "close") {
      *panel = p;
      *index = -1;
      return true;
    }
    if (rest.size() == 2 && rest[0] == 'b' && rest[1] >= '0' && rest[1] <= '7') {
      *panel = p;
      *index = rest[1] - '0';
      return true;
    }
  }
  return false;
}

}  // namespace hud
