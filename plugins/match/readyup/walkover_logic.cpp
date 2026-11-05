#include "readyup/walkover_logic.h"

#include <algorithm>

namespace readyup {
namespace {

std::string L(const char* k, const std::string& v) { return std::string(k) + "=" + v + "\n"; }

// A spec value: no line breaks, no '|' (it separates button fields).
std::string Text(const std::string& s) {
  std::string out;
  for (char ch : s) {
    if (ch == '\n' || ch == '\r') out += ' ';
    else if (ch != '|') out += ch;
  }
  return out;
}

}  // namespace

std::string WalkoverSpec(const WalkoverView& v) {
  const std::string secs = std::to_string(std::max(0LL, v.secondsLeft));
  const std::string shortName = Text(v.shortName.empty() ? "The other team" : v.shortName);
  const std::string fullName = Text(v.fullName.empty() ? "The other team" : v.fullName);
  std::string s = L("id", "walkover") + L("color", "warn") + L("timer", secs);
  if (v.onFullTeam && v.chooser) {
    return s + L("kind", shortName + " is short") + L("title", "Take the walkover?") +
           L("sub", "Or play them short. No answer: the walkover stands.") + "btn=Take the walkover|green||.walkover\n" +
           "btn=Play them short|ghost||.play\n" + L("bw", "l") + L("cmd", "Or type|.walkover / .play") + L("mouse", "on");
  }
  if (v.onFullTeam) {
    return s + L("kind", shortName + " is short") + L("title", "Your captain chooses") +
           L("sub", "Take the walkover, or play them short.");
  }
  if (v.onShortTeam) {
    return s + L("kind", "Time is up") + L("title", fullName + " chooses") +
           L("sub", "They take the walkover, or play you short.");
  }
  return s + L("kind", shortName + " is short") + L("title", fullName + " chooses");
}

long long SecondsUntil(long long untilMs, long long nowMs) {
  if (untilMs <= 0) return -1;
  return std::max(0LL, (untilMs - nowMs + 999) / 1000);
}

status::Json WalkoverChoiceData(uint64_t steamid64, const std::string& name, bool take) {
  status::Json player = status::Json::Object();
  player["steamid64"] = std::to_string(steamid64);
  player["name"] = name;
  status::Json d = status::Json::Object();
  d["player"] = std::move(player);
  d["take"] = take;
  return d;
}

}  // namespace readyup
