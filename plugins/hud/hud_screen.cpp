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

const char* const kColours[] = {"c-live", "c-warn", "c-ban", "c-muted", "c-ct", "c-t"};
// How the centre panel enters (CS2 motion, at_ui.css): a decision flips in, an alert flashes,
// everything else opens from the centre. The card always slides in from the right.
const char* const kFx[] = {"fx-open", "fx-flip", "fx-alert", "fx-slide", "fx-slide-left", "fx-pop",
                           "fx-zoom", "fx-fade", "fx-rise", "fx-drop", "fx-none"};
const char* const kParts[] = {"brand", "kind", "title", "sub", "hint", "timer", "counter", "teams", "confirm",
                              "lines", "cmds", "buttons", "close", "flow", "b0", "b1", "b2", "b3", "b4", "b5", "b6", "b7"};

bool UtilityName(const std::string& c) {
  if (c.empty() || c.size() > 32) return false;
  for (char ch : c)
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-')) return false;
  return true;
}
const char* const kTones[] = {"t-green", "t-ct", "t-t", "t-warn", "t-danger"};
const char* const kWidths[] = {"bw-xs", "bw-s", "bw-m", "bw-l", "bw-xl"};

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
      if (s.buttons.size() < static_cast<size_t>(kMaxButtons)) s.buttons.push_back(Button{At(p, 0), At(p, 1), At(p, 2), At(p, 3), At(p, 4)});
    } else if (k == "cols") {
      s.cols = std::clamp(std::atoi(v.c_str()), 1, 6);
    } else if (k == "bw") {
      if (v == "xs" || v == "s" || v == "m" || v == "l" || v == "xl") s.bw = v;
    } else if (k == "close") {
      const auto p = SplitBar(v);
      s.hasClose = !At(p, 0).empty();
      s.closeLabel = At(p, 0);
      s.closeCmd = At(p, 1);
    } else if (k == "mouse") {
      s.mouse = v == "on" ? Mouse::On : v == "b" ? Mouse::B : Mouse::Off;
    } else if (k == "line") {
      const auto p = SplitBar(v);
      if (s.lines.size() < 5) s.lines.emplace_back(At(p, 0), At(p, 1));
    } else if (k == "size") {
      s.mini = v == "mini";
    } else if (k == "fx" || k == "enter") {
      s.fx = v;
    } else if (k == "exit") {
      s.exit = v;
    } else if (k == "cls" || k.rfind("cls.", 0) == 0) {
      const std::string part = k.size() > 4 ? k.substr(4) : std::string();
      size_t a = 0;
      while (a < v.size()) {
        size_t b = v.find(' ', a);
        if (b == std::string::npos) b = v.size();
        const std::string c = v.substr(a, b - a);
        if (UtilityName(c) && s.utils.size() < 48) s.utils.emplace_back(part, c);
        a = b + 1;
      }
    } else if (k == "wash") {
      s.wash = v != "0";
    } else if (k == "pixels") {
      s.pixels = v != "0";
    } else if (k == "blur") {
      s.blur = v != "0";
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

  cls(p, "mini", s.mini);
  const std::string fx = EnterOf(s, p);
  for (const char* f : kFx) cls(p, f, fx == f);
  const int secs = s.timer.empty() ? -1 : std::atoi(s.timer.c_str());
  cls(p, "no-wash", !s.wash);
  cls(p, "no-px", !s.pixels);
  cls(p, "blur", s.blur);
  cls(p, "urgent", secs >= 0 && secs <= 5 && s.timer.find_first_not_of("0123456789") == std::string::npos);
  cls(p, "has-lines", !s.lines.empty());
  for (size_t i = 0; i < 5; ++i) {
    const std::string r = p + "_n" + std::to_string(i);
    const std::string v = i < s.lines.size() ? s.lines[i].second : std::string();
    text(r + "_k", i < s.lines.size() ? s.lines[i].first : std::string());
    text(r + "_v", v);
    cls(r, "gone", i >= s.lines.size());
    cls(r + "_v", "cmd", !v.empty() && v[0] == '.');
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
  for (int c = 1; c <= 6; ++c) cls(p, "cols-" + std::to_string(c), c == cols);
  for (const char* bw : kWidths) cls(p, bw, std::string("bw-") + s.bw == bw);
  for (size_t i = 0; i < static_cast<size_t>(kMaxButtons); ++i) {
    const std::string b = p + "_b" + std::to_string(i);
    const Button* btn = i < nb ? &s.buttons[i] : nullptr;
    text(b + "_l", btn ? btn->label : std::string());
    text(b + "_s", btn ? btn->sub : std::string());
    cls(b, "show", btn != nullptr);
    cls(b, "has-sub", btn && !btn->sub.empty());
    const std::string tone = btn ? "t-" + btn->tone : std::string();
    for (const char* t : kTones) cls(b, t, btn && btn->state != "off" && tone == t);
    cls(b, "picked", btn && btn->state == "picked");
    cls(b, "pulse", btn && btn->state == "pulse");
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
    if (rest.size() >= 2 && rest.size() <= 3 && rest[0] == 'b' &&
        rest.find_first_not_of("0123456789", 1) == std::string::npos) {
      const int i = std::atoi(rest.c_str() + 1);
      if (i >= kMaxButtons) return false;
      *panel = p;
      *index = i;
      return true;
    }
  }
  return false;
}

std::string EnterOf(const Screen& s, const std::string& p) {
  const std::string want = "fx-" + s.fx;
  for (const char* f : kFx)
    if (want == f) return want;
  const bool decision = s.mouse == Mouse::On && !s.buttons.empty() && !s.hasClose;
  if (s.mini) return "fx-pop";
  if (p == "ui_card") return s.color == "ban" ? "fx-alert" : "fx-slide";
  return decision ? "fx-flip" : s.color == "ban" ? "fx-alert" : "fx-open";
}

const std::vector<std::string>& ExitClasses() {
  static const std::vector<std::string> k = {"out-fade", "out-close", "out-slide", "out-slide-left", "out-shrink",
                                             "out-drop", "out-rise", "out-flip", "out-none"};
  return k;
}

std::string ExitOf(const Screen& s, const std::string& p) {
  const std::string want = "out-" + s.exit;
  for (const auto& c : ExitClasses())
    if (want == c) return want;
  const std::string in = EnterOf(s, p);
  if (in == "fx-open") return "out-close";
  if (in == "fx-slide") return "out-slide";
  if (in == "fx-slide-left") return "out-slide-left";
  if (in == "fx-pop" || in == "fx-zoom") return "out-shrink";
  if (in == "fx-none") return "out-none";
  return "out-fade";
}

std::vector<std::pair<std::string, std::string>> Utilities(const Screen& s, const std::string& p) {
  std::vector<std::pair<std::string, std::string>> out;
  for (const auto& [part, c] : s.utils) {
    if (part.empty()) {
      out.emplace_back(p, c);
      continue;
    }
    for (const char* known : kParts)
      if (part == known) out.emplace_back(p + "_" + part, c);
  }
  return out;
}

}  // namespace hud
