// libs/readyup/i18n_api.h against a fake ru_api: one chat_all when everyone shares a
// language, one line per player otherwise, "en" on a core before 1.19. ctest `i18n_api`.
#include "readyup/i18n_api.h"

#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK_EQ(a, b)                                                                              \
  do {                                                                                              \
    const std::string x_ = (a), y_ = (b);                                                           \
    if (x_ != y_) {                                                                                 \
      std::fprintf(stderr, "%s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, x_.c_str(), y_.c_str()); \
      ++g_failures;                                                                                 \
    }                                                                                               \
  } while (0)

std::vector<std::string> g_langs;  // by slot
std::vector<std::string> g_sent;   // "all:<msg>" or "<slot>:<msg>"

int ForEach(ru_plugin*, ru_player_fn fn, void* user) {
  for (size_t i = 0; i < g_langs.size(); ++i) {
    ru_player p{};
    p.struct_size = sizeof(p);
    p.slot = static_cast<int>(i);
    p.connected = 1;
    if (!fn(user, &p)) break;
  }
  return static_cast<int>(g_langs.size());
}
std::vector<uint64_t> g_sids;  // by slot, for get_player
int GetPlayer(ru_plugin*, int slot, ru_player* out) {
  if (slot < 0 || slot >= static_cast<int>(g_sids.size()) || g_sids[slot] == 0) return 0;
  out->slot = slot;
  out->steamid64 = g_sids[slot];
  return 1;
}
const char* Lang(ru_plugin*, int slot) { return slot < 0 ? "en" : g_langs[slot].c_str(); }
int ChatAll(ru_plugin*, const char* msg, uint32_t) {
  g_sent.push_back(std::string("all:") + msg);
  return 1;
}
int ChatSlot(ru_plugin*, int slot, const char* msg, uint32_t) {
  g_sent.push_back(std::to_string(slot) + ":" + msg);
  return 1;
}

std::string Sent() {
  std::string s;
  for (const auto& l : g_sent) s += (s.empty() ? "" : " | ") + l;
  g_sent.clear();
  return s;
}

}  // namespace

int main() {
  ru_api api{};
  api.struct_size = sizeof(api);
  api.for_each_player = &ForEach;
  api.chat_all = &ChatAll;
  api.player_language = &Lang;
  api.chat_to_slot_ex = &ChatSlot;
  api.get_player = &GetPlayer;
  auto hello = [](const std::string& lang) { return "hi-" + lang; };

  g_langs = {"de", "de"};
  readyup::i18n::ChatAll(&api, 0, hello);
  CHECK_EQ(Sent(), "all:hi-de");

  g_langs = {"de", "fr", "de"};
  readyup::i18n::ChatAll(&api, 0, hello);
  CHECK_EQ(Sent(), "0:hi-de | 1:hi-fr | 2:hi-de");

  g_langs = {};
  readyup::i18n::ChatAll(&api, 0, hello);
  CHECK_EQ(Sent(), "all:hi-en");  // nobody on: the server's language

  ru_api old = api;
  old.struct_size = offsetof(ru_api, player_language);  // a 1.18 core
  g_langs = {"de", "fr"};
  readyup::i18n::ChatAll(&old, 0, hello);
  CHECK_EQ(Sent(), "all:hi-en");
  CHECK_EQ(readyup::i18n::LanguageOf(&old, 0), "en");

  // LangSeen: a card drawn in de is stale once the player switches to fr; one never drawn is not,
  // nor one drawn for whoever had the slot before.
  readyup::i18n::LangSeen seen;
  auto changed = [&](int slot, uint64_t sid) { return seen.Changed(&api, slot, sid) ? "changed" : "same"; };
  g_langs = {"de", "de"};
  seen.Drew(0, 7, "de");
  CHECK_EQ(changed(0, 7), "same");
  CHECK_EQ(changed(1, 7), "same");
  g_langs = {"fr", "fr"};
  CHECK_EQ(changed(0, 7), "changed");
  CHECK_EQ(changed(0, 8), "same");
  seen.Drew(0, 7, "fr");
  CHECK_EQ(changed(0, 7), "same");
  seen.Drew(1, 7, "de");
  seen.Forget(1);
  CHECK_EQ(changed(1, 7), "same");

  // Stale: what a menu redraws. Slot 0 switched (still player 7), slot 1 has a new player.
  auto stale = [&] {
    std::string s;
    for (const auto& [slot, sid] : seen.Stale(&api)) s += std::to_string(slot) + "/" + std::to_string(sid) + " ";
    return s;
  };
  seen.by_slot.clear();
  g_langs = {"de", "de", "de"};
  g_sids = {7, 9, 11};
  seen.Drew(0, 7, "de");
  seen.Drew(1, 8, "de");
  seen.Drew(2, 11, "de");
  CHECK_EQ(stale(), "");
  CHECK_EQ(std::to_string(seen.by_slot.size()), "2");  // slot 1's player left: forgotten
  g_langs = {"fr", "de", "de"};
  CHECK_EQ(stale(), "0/7 ");

  if (g_failures) {
    std::fprintf(stderr, "i18n_api: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("i18n_api: ok\n");
  return 0;
}
