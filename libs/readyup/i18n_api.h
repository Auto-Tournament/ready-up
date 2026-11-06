#pragma once

// i18n.h on top of ru_api (1.19): the player's language and chat to everyone, each player in
// their own. Header only; on a core older than 1.19 everyone gets "en" through chat_all.

#include "readyup/plugin_api.h"

#include <cstdint>
#include <map>
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

// The language each player's held card (a menu, a card shown for some seconds) was drawn in:
// Changed() says when that player's language has moved on since, so the card is drawn again.
// Keyed by slot and SteamID: someone new in the slot is not the player who saw the card.
struct LangSeen {
  struct Seen {
    std::string lang;
    uint64_t steamid64 = 0;
  };
  std::map<int, Seen> by_slot;
  void Drew(int slot, uint64_t steamid64, const std::string& lang) { by_slot[slot] = Seen{lang, steamid64}; }
  void Forget(int slot) { by_slot.erase(slot); }
  bool Changed(const ru_api* api, int slot, uint64_t steamid64) const {
    const auto it = by_slot.find(slot);
    return it != by_slot.end() && it->second.steamid64 == steamid64 && it->second.lang != LanguageOf(api, slot);
  }
  // Each tick, for menus: the slots (and SteamIDs) to draw again. Someone else in a slot (the
  // player left) is forgotten instead.
  std::vector<std::pair<int, uint64_t>> Stale(const ru_api* api) {
    std::vector<std::pair<int, uint64_t>> out;
    for (auto it = by_slot.begin(); it != by_slot.end();) {
      ru_player p{};
      p.struct_size = sizeof(p);
      if (!api->get_player(api->self, it->first, &p) || p.steamid64 != it->second.steamid64) {
        it = by_slot.erase(it);
        continue;
      }
      if (it->second.lang != LanguageOf(api, it->first)) out.emplace_back(it->first, p.steamid64);
      ++it;
    }
    return out;
  }
};

}  // namespace readyup::i18n
