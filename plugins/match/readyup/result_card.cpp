#include "readyup/result_card.h"

#include "readyup/hud_screens.h"
#include "readyup/admin_call.h"
#include "readyup/config.h"
#include "readyup/end_screen.h"
#include "readyup/engine.h"
#include "readyup/host.h"
#include "readyup/i18n_api.h"
#include "readyup/logging.h"
#include "readyup/players.h"
#include "readyup/plugin_api.h"

#include <chrono>
#include <mutex>
#include <unordered_map>

namespace readyup {
namespace {

struct Card {
  ResultCardBuild build;
  std::unordered_map<std::string, std::pair<std::string, std::string>> byLang;  // html, spec
  int64_t untilMs = 0;
  int64_t lastSendMs = 0;
};

std::mutex g_mu;
Card g_card;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

void ResultCardShow(ResultCardBuild build, int seconds, const char* what) {
  if (!build || seconds <= 0) return;
  std::lock_guard<std::mutex> lk(g_mu);
  g_card = Card{};
  g_card.build = std::move(build);
  g_card.untilMs = NowMs() + static_cast<int64_t>(seconds) * 1000;
  if (DebugEnabled()) Debug("result-card: %s for %ds\n", what ? what : "card", seconds);
}

void ResultCardClear() {
  std::lock_guard<std::mutex> lk(g_mu);
  g_card = Card{};
}

void ResultCardTick() {
  EndScreenTick();  // the leaderboard's countdown and its end (end_screen.h)
  ResultCardBuild build;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_card.build) return;
    const int64_t now = NowMs();
    if (now >= g_card.untilMs) {
      g_card = Card{};
      return;
    }
    if (g_card.lastSendMs != 0 && now - g_card.lastSendMs < Cfg().hud_resend_ms) return;
    g_card.lastSendMs = now;
    build = g_card.build;
  }
  const bool screensOn = HudScreensOn();
  for (const auto& h : ListHumans()) {
    const int slot = h.slot >= 0 ? h.slot : h.userid;
    if (h.steamid64 == 0 || slot < 0 || slot > 63) continue;
    if (AdminCallCardActiveFor(slot)) continue;
    const std::string lang = i18n::LanguageOf(host::Api(), slot);
    std::pair<std::string, std::string> card;
    {
      std::lock_guard<std::mutex> lk(g_mu);
      auto it = g_card.byLang.find(lang);
      if (it == g_card.byLang.end()) it = g_card.byLang.emplace(lang, build(lang)).first;
      card = it->second;
    }
    const std::string& html = card.first;
    const std::string& spec = card.second;
    const bool screens = screensOn && !spec.empty();
    if (screens) HudScreensNotice(slot, spec, 1.0);
    else (void)SendCenterHtml(slot, html, Cfg().hud_duration_s, RU_HTML_PRIO_NOTICE);
  }
}

}  // namespace readyup
