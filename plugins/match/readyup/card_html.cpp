#include "readyup/card_html.h"
#include "readyup/text.h"

#include <algorithm>

namespace readyup {
namespace {

std::string T(const std::string& lang, const char* key, const text::Args& args = {}) {
  return text::ForLang(lang, key, args);
}

// "de_mirage" -> "Mirage", "cs_office" -> "Office", "workshop/123/de_x" -> "X".
std::string PrettyMap(const std::string& map) {
  std::string m = map;
  const size_t slash = m.find_last_of('/');
  if (slash != std::string::npos) m = m.substr(slash + 1);
  for (const char* p : {"de_", "cs_", "ar_", "dz_"}) {
    if (m.rfind(p, 0) == 0) {
      m = m.substr(3);
      break;
    }
  }
  for (char& c : m) {
    if (c == '_') c = ' ';
  }
  if (!m.empty() && m[0] >= 'a' && m[0] <= 'z') m[0] = static_cast<char>(m[0] - 'a' + 'A');
  return m;
}

text::Args DamageArgs(const ResultCardInfo& info) {
  return {{"damage1", std::to_string(info.team1Damage)}, {"damage2", std::to_string(info.team2Damage)}};
}

}  // namespace

std::string GoLiveCommandsLine(const GoLiveCardInfo& info, const std::string& lang) {
  std::string out;
  // What it does first, then the command: ".up resume" read like a command with an argument.
  auto add = [&](const char* whatKey, const char* cmd) {
    if (!out.empty()) out += " | ";
    out += T(lang, "card.command", {{"what", T(lang, whatKey)}, {"command", cmd}});
  };
  if (info.pauses) {
    add("card.pause", ".p");
    add("card.unpause", ".up");
    add("card.timeout", ".tac");
  }
  if (info.adminCall) add("card.call_admin", ".admin");
  return out;
}

namespace {

// One spec value: no newlines, no '|', no control bytes (chat colours).
std::string SpecText(const std::string& in) {
  std::string out;
  for (char c : in) {
    if (c == '\n' || c == '\r') out += ' ';
    else if (c != '|' && static_cast<unsigned char>(c) >= 0x20) out += c;
  }
  return out;
}
std::string SpecLine(const char* k, const std::string& v) { return std::string(k) + "=" + SpecText(v) + "\n"; }
std::string RawTeam(const ResultCardInfo& info, const std::string& which, const std::string& lang) {
  const std::string& n = which == "team1" ? info.team1 : info.team2;
  return n.empty() ? T(lang, which == "team1" ? "card.team1" : "card.team2") : n;
}

}  // namespace

std::string GoLiveCardSpec(const GoLiveCardInfo& info, const std::string& lang) {
  std::string s = SpecLine("id", "golive") + SpecLine("color", "live") + SpecLine("kind", T(lang, "card.kind_live")) +
                  SpecLine("title", T(lang, "card.live_title"));  // "Match is live" (Sivert, 2026-11-24)
  if (!info.team1.empty() || !info.team2.empty()) {
    // The names in their side's colour say who is CT and T (2026-11-03: "Simpert (CT) vs Bots (T)").
    const int side2 = info.team1Side == 3 ? 2 : info.team1Side == 2 ? 3 : 0;
    auto side = [](int s) { return s == 3 ? "ct" : s == 2 ? "t" : ""; };
    s += std::string("vs=") + SpecText(info.team1.empty() ? T(lang, "card.team1") : info.team1) + "|" + side(info.team1Side) + "|" +
         SpecText(info.team2.empty() ? T(lang, "card.team2") : info.team2) + "|" + side(side2) + "\n";
  }
  // One short line: "[.menu] in chat for commands" (Sivert, 2026-11-24: not three commands, few words).
  s += "cmd=|.menu|" + SpecText(T(lang, "card.menu_after")) + "\n";
  return s;
}

std::string MapResultCardSpec(const ResultCardInfo& info, const std::string& lang) {
  std::string kind = T(lang, "card.map_kind", {{"number", std::to_string(std::max(1, info.mapNumber))}});
  if (!info.mapName.empty()) kind += " \xC2\xB7 " + PrettyMap(info.mapName);
  const bool won = info.winner == "team1" || info.winner == "team2";
  std::string s = SpecLine("id", "mapresult") + SpecLine("color", "live") + SpecLine("kind", kind) +
                  SpecLine("title", won ? T(lang, "card.team_wins_map", {{"team", RawTeam(info, info.winner, lang)}}) : T(lang, "card.map_drawn_title")) +
                  SpecLine("sub", std::to_string(info.team1Score) + " - " + std::to_string(info.team2Score));
  if (info.decidedByDamage) {
    s += SpecLine("hint", T(lang, "card.tied_damage", DamageArgs(info)));
  } else if (!info.nextMap.empty()) {
    s += SpecLine("hint", T(lang, "card.next", {{"map", PrettyMap(info.nextMap)}}));
  }
  return s;
}

std::string SeriesResultCardSpec(const ResultCardInfo& info, const std::string& lang) {
  const bool won = info.winner == "team1" || info.winner == "team2";
  return SpecLine("id", "seriesresult") + SpecLine("color", "live") + SpecLine("kind", T(lang, "card.kind_series")) +
         SpecLine("title", won ? T(lang, "card.team_wins_series", {{"team", RawTeam(info, info.winner, lang)}}) : T(lang, "card.series_drawn_title")) +
         SpecLine("sub", info.forfeit ? T(lang, "card.by_forfeit")
                                      : std::to_string(info.team1Score) + " - " + std::to_string(info.team2Score));
}

std::string AdminCallCardSpec(const std::string& name, const std::string& teamLabel, const std::string& message,
                              const std::string& lang) {
  std::string who = name.empty() ? T(lang, "card.a_player_cap") : name;
  if (!teamLabel.empty()) who += " (" + teamLabel + ")";
  // It may come mid-round: the side card, out of the way (hud_screens.h placement rule).
  std::string s = SpecLine("id", "admincall") + SpecLine("where", "card") + SpecLine("color", "ban") +
                  SpecLine("kind", T(lang, "card.admin_called")) + SpecLine("title", T(lang, "card.needs_admin", {{"who", who}}));
  if (!message.empty()) s += SpecLine("sub", message.substr(0, 120));
  return s;
}

}  // namespace readyup
