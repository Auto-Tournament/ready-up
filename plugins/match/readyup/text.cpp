#include "readyup/text.h"

#include "readyup/engine.h"
#include "readyup/host.h"
#include "readyup/i18n_api.h"
#include "readyup/logging.h"

#include <utility>
#include <vector>

extern const readyup::i18n::Locale kReadyUpLocales[];  // plugins/match/locales (readyup_embed_locales)
extern const size_t kReadyUpLocaleCount;

namespace readyup::text {
namespace {

const i18n::Catalog& Catalog() {
  static const i18n::Catalog c = [] {
    i18n::Catalog x;
    if (const std::string err = x.Load(kReadyUpLocales, kReadyUpLocaleCount); !err.empty())
      Print("match: locales: %s\n", err.c_str());
    return x;
  }();
  return c;
}

// The center panel of SendImportantToChat for one language.
std::string GoldHtml(const std::string& text) {
  std::string esc;
  for (char ch : text) {
    if (ch == '&') esc += "&amp;";
    else if (ch == '<') esc += "&lt;";
    else if (ch == '>') esc += "&gt;";
    else esc.push_back(ch);
  }
  return "<font color='#FFD700'>" + esc + "</font>";
}

}  // namespace

std::string ForLang(const std::string& lang, const std::string& key, const Args& args) {
  return Catalog().Tr(lang, key, args);
}

std::string For(int slot, const std::string& key, const Args& args) {
  return ForLang(i18n::LanguageOf(host::Api(), slot), key, args);
}

void ChatAll(const std::string& key, Args args) {
  host::RunOnGameThread([key, args = std::move(args)] {
    if (const ru_api* a = host::Api()) i18n::ChatAll(a, 0, [&](const std::string& lang) { return ForLang(lang, key, args); });
  });
}

void ImportantAll(const std::string& key, Args args, int centerSeconds) {
  host::RunOnGameThread([key, args = std::move(args), centerSeconds] {
    const ru_api* a = host::Api();
    if (!a) return;
    i18n::ChatAll(a, RU_CHAT_IMPORTANT, [&](const std::string& lang) { return ForLang(lang, key, args); });
    if (centerSeconds <= 0) return;
    std::vector<int> slots;
    a->for_each_player(
        a->self,
        [](void* user, const ru_player* p) {
          if (p->connected && !p->is_bot && p->slot >= 0) static_cast<std::vector<int>*>(user)->push_back(p->slot);
          return 1;
        },
        &slots);
    for (int slot : slots) SendCenterHtml(slot, GoldHtml(For(slot, key, args)), centerSeconds, RU_HTML_PRIO_ALERT);
  });
}

void ChatSlot(int slot, const std::string& key, Args args) {
  host::RunOnGameThread([slot, key, args = std::move(args)] {
    ClientPrintChat(slot, (" \x04[ReadyUp]\x01 " + For(slot, key, args)).c_str());
  });
}

}  // namespace readyup::text
