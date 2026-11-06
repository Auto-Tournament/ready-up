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

  // LangSeen: a card drawn in de is stale once the player switches to fr; one never drawn is not.
  readyup::i18n::LangSeen seen;
  g_langs = {"de", "de"};
  seen.Drew(0, "de");
  CHECK_EQ(seen.Changed(&api, 0) ? "changed" : "same", "same");
  CHECK_EQ(seen.Changed(&api, 1) ? "changed" : "same", "same");
  g_langs = {"fr", "fr"};
  CHECK_EQ(seen.Changed(&api, 0) ? "changed" : "same", "changed");
  seen.Drew(0, "fr");
  CHECK_EQ(seen.Changed(&api, 0) ? "changed" : "same", "same");
  seen.Drew(1, "de");
  seen.Forget(1);
  CHECK_EQ(seen.Changed(&api, 1) ? "changed" : "same", "same");

  if (g_failures) {
    std::fprintf(stderr, "i18n_api: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("i18n_api: ok\n");
  return 0;
}
