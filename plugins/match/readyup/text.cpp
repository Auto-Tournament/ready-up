#include "readyup/text.h"

#include "readyup/engine.h"
#include "readyup/host.h"
#include "readyup/hud_screens.h"
#include "readyup/i18n_api.h"
#include "readyup/logging.h"

#include <utility>
#include <vector>

namespace readyup::text {
namespace {

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

std::string For(int slot, const std::string& key, const Args& args) {
  return ForLang(i18n::LanguageOf(host::Api(), slot), key, args);
}

void ChatAll(const std::string& key, Args args) {
  ChatAllFn([key, args = std::move(args)](const std::string& lang) { return ForLang(lang, key, args); });
}

void ChatAllFn(TextFn text) {
  host::RunOnGameThread([text = std::move(text)] {
    if (const ru_api* a = host::Api()) i18n::ChatAll(a, 0, text);
  });
}

void ImportantAll(const std::string& key, Args args, int centerSeconds) {
  ImportantAllFn([key, args = std::move(args)](const std::string& lang) { return ForLang(lang, key, args); }, centerSeconds);
}

namespace {
// flags: RU_CHAT_IMPORTANT (star lines: tips) or 0 ([Ready Up] line: a match event).
void AllWithCenter(TextFn text, int centerSeconds, int flags) {
  host::RunOnGameThread([text = std::move(text), centerSeconds, flags] {
    const ru_api* a = host::Api();
    if (!a) return;
    i18n::ChatAll(a, flags, text);
    // The gold centre panel only without the clickable HUD; with it the chat line is enough
    // (the old centre HTML sat over the crosshair; Sivert, 2026-11-25).
    if (centerSeconds <= 0 || HudScreensOn()) return;
    std::vector<int> slots;
    a->for_each_player(
        a->self,
        [](void* user, const ru_player* p) {
          if (p->connected && !p->is_bot && p->slot >= 0) static_cast<std::vector<int>*>(user)->push_back(p->slot);
          return 1;
        },
        &slots);
    for (int slot : slots) SendCenterHtml(slot, GoldHtml(text(i18n::LanguageOf(a, slot))), centerSeconds, RU_HTML_PRIO_ALERT);
  });
}
}  // namespace

void ImportantAllFn(TextFn text, int centerSeconds) { AllWithCenter(std::move(text), centerSeconds, RU_CHAT_IMPORTANT); }

void EventAll(const std::string& key, Args args, int centerSeconds) {
  EventAllFn([key, args = std::move(args)](const std::string& lang) { return ForLang(lang, key, args); }, centerSeconds);
}

void EventAllFn(TextFn text, int centerSeconds) { AllWithCenter(std::move(text), centerSeconds, 0); }

void ChatSlot(int slot, const std::string& key, Args args) {
  host::RunOnGameThread([slot, key, args = std::move(args)] {
    // The server's prefix (light red [Ready Up] by default), like every other Ready Up line; this
    // one had its own green "[ReadyUp]".
    const ru_api* a = host::Api();
    if (a) a->chat_to_slot(a->self, slot, For(slot, key, args).c_str());
  });
}

}  // namespace readyup::text
