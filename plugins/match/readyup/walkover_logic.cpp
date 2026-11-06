#include "readyup/walkover_logic.h"

#include "readyup/text.h"

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

std::string WalkoverSpec(const WalkoverView& v, const std::string& lang) {
  auto t = [&](const char* key, const text::Args& args = {}) { return Text(text::ForLang(lang, key, args)); };
  const std::string secs = std::to_string(std::max(0LL, v.secondsLeft));
  const std::string other = text::ForLang(lang, "walkover.other_team");
  const std::string shortName = v.shortName.empty() ? other : v.shortName;
  const std::string fullName = v.fullName.empty() ? other : v.fullName;
  const std::string isShort = t("walkover.is_short", {{"team", shortName}});
  std::string s = L("id", "walkover") + L("color", "warn") + L("timer", secs);
  if (v.onFullTeam && v.chooser) {
    return s + L("kind", isShort) + L("title", t("walkover.take_q")) + L("sub", t("walkover.take_sub")) + "btn=" +
           t("walkover.btn_take") + "|green||.walkover\n" + "btn=" + t("walkover.btn_short") + "|ghost||.play\n" +
           L("bw", "l") + L("cmd", t("walkover.or_type") + "|.walkover / .play") + L("mouse", "on");
  }
  if (v.onFullTeam) return s + L("kind", isShort) + L("title", t("walkover.captain")) + L("sub", t("walkover.captain_sub"));
  if (v.onShortTeam) {
    return s + L("kind", t("walkover.time_up")) + L("title", t("walkover.chooses", {{"team", fullName}})) +
           L("sub", t("walkover.they_choose"));
  }
  return s + L("kind", isShort) + L("title", t("walkover.chooses", {{"team", fullName}}));
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
