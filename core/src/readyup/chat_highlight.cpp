#include "readyup/chat_highlight.h"

#include "readyup/chat_colors.h"

#include <cctype>

namespace readyup {

std::string HighlightChatText(const std::string& text) {
  for (unsigned char c : text) {
    if (c < 0x20) return text;  // colored already: the sender chose
  }
  // Numbers stay plain: gold was hard to read over a bright map (2026-11-03).
  const std::string green(1, ChatColors::Green), plain(1, ChatColors::Default);
  auto isWordChar = [](unsigned char c) { return std::isalnum(c) || c == '_'; };
  std::string out;
  out.reserve(text.size() + 16);
  size_t i = 0;
  while (i < text.size()) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    const bool atWordStart = i == 0 || !isWordChar(static_cast<unsigned char>(text[i - 1]));
    // .ready, .pause, .tech: a chat command (a dot at a word start, then letters).
    if (c == '.' && atWordStart && i + 1 < text.size() && std::isalpha(static_cast<unsigned char>(text[i + 1]))) {
      size_t j = i + 1;
      while (j < text.size() && isWordChar(static_cast<unsigned char>(text[j]))) ++j;
      out += green + text.substr(i, j - i) + plain;
      i = j;
      continue;
    }
    out.push_back(static_cast<char>(c));
    ++i;
  }
  return out;
}

std::string WithoutReadyUpLeadIn(const std::string& text) {
  static const std::string lead = "Ready Up: ";
  return text.rfind(lead, 0) == 0 ? text.substr(lead.size()) : text;
}

}  // namespace readyup
