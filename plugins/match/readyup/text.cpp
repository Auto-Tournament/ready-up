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

void ImportantAll(const std::string& key, Args args) {
  ImportantAllFn([key, args = std::move(args)](const std::string& lang) { return ForLang(lang, key, args); });
}

namespace {
// flags: RU_CHAT_IMPORTANT (star lines: tips) or 0 ([Ready Up] line: a match event). Chat only:
// the gold centre HTML is gone (2026-11-26); the HUD shows its own notice where it matters.
void AllLines(TextFn text, int flags) {
  host::RunOnGameThread([text = std::move(text), flags] {
    if (const ru_api* a = host::Api()) i18n::ChatAll(a, flags, text);
  });
}
}  // namespace

void ImportantAllFn(TextFn text) { AllLines(std::move(text), RU_CHAT_IMPORTANT); }

void EventAll(const std::string& key, Args args) {
  EventAllFn([key, args = std::move(args)](const std::string& lang) { return ForLang(lang, key, args); });
}

void EventAllFn(TextFn text) { AllLines(std::move(text), 0); }

void ChatSlot(int slot, const std::string& key, Args args) {
  host::RunOnGameThread([slot, key, args = std::move(args)] {
    // The server's prefix (light red [Ready Up] by default), like every other Ready Up line; this
    // one had its own green "[ReadyUp]".
    const ru_api* a = host::Api();
    if (a) a->chat_to_slot(a->self, slot, For(slot, key, args).c_str());
  });
}

}  // namespace readyup::text
