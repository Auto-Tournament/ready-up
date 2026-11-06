#pragma once

// i18n.h on top of ru_api (1.19): the player's language and chat to everyone, each player in
// their own. Header only; on a core older than 1.19 everyone gets "en" through chat_all.

#include "readyup/plugin_api.h"

#include <string>
#include <utility>
#include <vector>

namespace readyup::i18n {

// The language of the player in `slot`; -1 = the server's.
inline const char* LanguageOf(const ru_api* api, int slot) {
  if (RU_API_HAS(api, player_language) && api->player_language) return api->player_language(api->self, slot);
  return "en";
}

// Sends text(lang) to everyone (flags: RU_CHAT_*). When every player has the same language
// it is one chat_all, as before (GOTV and demos get it too); otherwise each player gets the
// line in their own language and GOTV gets none.
template <class TextFn>
void ChatAll(const ru_api* api, uint32_t flags, TextFn text) {
  std::vector<std::pair<int, std::string>> players;  // slot, language
  if (RU_API_HAS(api, chat_to_slot_ex) && api->chat_to_slot_ex) {
    api->for_each_player(
        api->self,
        [](void* user, const ru_player* p) {
          if (p->connected && !p->is_bot && p->slot >= 0) static_cast<decltype(players)*>(user)->emplace_back(p->slot, "");
          return 1;
        },
        &players);
    for (auto& p : players) p.second = LanguageOf(api, p.first);
  }
  bool same = true;
  for (const auto& p : players) same = same && p.second == players.front().second;
  if (same) {
    const std::string line = text(std::string(players.empty() ? LanguageOf(api, -1) : players.front().second));
    api->chat_all(api->self, line.c_str(), flags);
    return;
  }
  for (const auto& p : players) api->chat_to_slot_ex(api->self, p.first, text(p.second).c_str(), flags);
}

}  // namespace readyup::i18n
