#include "readyup/card_html.h"

namespace readyup {
namespace {

// Palette (matches the welcome card and the ready HUD).
constexpr const char* kCt = "#5EA8FF";
constexpr const char* kT = "#FF9D3B";
constexpr const char* kGold = "#FFD23F";
constexpr const char* kGrey = "#8A8F98";
constexpr const char* kOk = "#7CD35A";
constexpr const char* kNo = "#F87171";
constexpr const char* kWhite = "#FFFFFF";

constexpr size_t kMaxTeamBytes = 24;
constexpr size_t kMaxNameBytes = 32;
constexpr size_t kMaxMessageBytes = 120;

void Font(std::string& h, const char* size, const char* color, const std::string& body) {
  h += "<font class='fontSize-";
  h += size;
  h += "' color='";
  h += color;
  h += "'>";
  h += body;
  h += "</font>";
}

std::string TeamPart(const std::string& name, int side) {
  std::string h;
  const char* color = side == 3 ? kCt : side == 2 ? kT : kWhite;
  std::string body = CardHtmlEscape(name, kMaxTeamBytes);
  if (side == 3) body += " (CT)";
  else if (side == 2) body += " (T)";
  Font(h, "m", color, body);
  return h;
}

}  // namespace

std::string CardHtmlEscape(const std::string& in, size_t maxBytes) {
  std::string s = in;
  if (s.size() > maxBytes) {
    size_t cut = maxBytes;
    // Don't split a UTF-8 sequence.
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    s.resize(cut);
    s += "...";
  }
  std::string out;
  out.reserve(s.size() + 16);
  for (char c : s) {
    switch (c) {
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '&': out += "&amp;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default:
        if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7F) out += c;  // drop control bytes (chat colors)
        break;
    }
  }
  return out;
}

std::string GoLiveCardHtml(const GoLiveCardInfo& info) {
  std::string h;
  h.reserve(800);
  Font(h, "xl", kOk, "<b>LIVE</b>");
  Font(h, "l", kGold, " &#183; GO GO GO");
  h += "<br>";
  if (!info.team1.empty() || !info.team2.empty()) {
    const int side2 = info.team1Side == 3 ? 2 : info.team1Side == 2 ? 3 : 0;
    h += TeamPart(info.team1.empty() ? std::string("Team 1") : info.team1, info.team1Side);
    Font(h, "m", kGrey, " vs ");
    h += TeamPart(info.team2.empty() ? std::string("Team 2") : info.team2, side2);
    h += "<br>";
  }
  if (info.pauses) {
    Font(h, "sm", kWhite, "<b>.p</b> / <b>.pause</b> / <b>.tech</b> pause &#183; <b>.up</b> / <b>.unpause</b> resume");
    h += "<br>";
  }
  std::string last;
  if (info.pauses) last += "<b>.tac</b> timeout";
  if (info.adminCall) {
    if (!last.empty()) last += " &#183; ";
    last += "<b>.admin</b> [message] call an admin";
  }
  if (!last.empty()) {
    Font(h, "sm", kWhite, last);
    h += "<br>";
  }
  Font(h, "s", kGrey, "good luck, have fun");
  return h;
}

std::string AdminCallCardHtml(const std::string& name, const std::string& teamLabel, const std::string& message) {
  std::string h;
  h.reserve(512);
  Font(h, "l", kNo, "<b>ADMIN CALLED</b>");
  h += "<br>";
  std::string who = CardHtmlEscape(name.empty() ? std::string("a player") : name, kMaxNameBytes);
  if (!teamLabel.empty()) who += " (" + CardHtmlEscape(teamLabel, kMaxTeamBytes + 8) + ")";
  Font(h, "m", kWhite, who + " needs an admin");
  if (!message.empty()) {
    h += "<br>";
    Font(h, "sm", kGold, CardHtmlEscape(message, kMaxMessageBytes));
  }
  return h;
}

}  // namespace readyup
