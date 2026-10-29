#include "readyup/result_card.h"

#include "readyup/hud_screens.h"
#include "readyup/admin_call.h"
#include "readyup/config.h"
#include "readyup/end_screen.h"
#include "readyup/engine.h"
#include "readyup/logging.h"
#include "readyup/players.h"
#include "readyup/plugin_api.h"

#include <chrono>
#include <mutex>

namespace readyup {
namespace {

struct Card {
  std::string html, spec;
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

void ResultCardShow(const std::string& html, const std::string& spec, int seconds, const char* what) {
  if (html.empty() || seconds <= 0) return;
  std::lock_guard<std::mutex> lk(g_mu);
  g_card = Card{};
  g_card.html = html;
  g_card.spec = spec;
  g_card.untilMs = NowMs() + static_cast<int64_t>(seconds) * 1000;
  if (DebugEnabled()) Debug("result-card: %s for %ds\n", what ? what : "card", seconds);
}

void ResultCardClear() {
  std::lock_guard<std::mutex> lk(g_mu);
  g_card = Card{};
}

void ResultCardTick() {
  EndScreenTick();  // the leaderboard's countdown and its end (end_screen.h)
  std::string html, spec;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_card.html.empty()) return;
    const int64_t now = NowMs();
    if (now >= g_card.untilMs) {
      g_card = Card{};
      return;
    }
    if (g_card.lastSendMs != 0 && now - g_card.lastSendMs < Cfg().hud_resend_ms) return;
    g_card.lastSendMs = now;
    html = g_card.html;
    spec = g_card.spec;
  }
  const bool screens = HudScreensOn() && !spec.empty();
  for (const auto& h : ListHumans()) {
    const int slot = h.slot >= 0 ? h.slot : h.userid;
    if (h.steamid64 == 0 || slot < 0 || slot > 63) continue;
    if (AdminCallCardActiveFor(slot)) continue;
    if (screens) HudScreensNotice(slot, spec, 1.0);
    else (void)SendCenterHtml(slot, html, Cfg().hud_duration_s, RU_HTML_PRIO_NOTICE);
  }
}

}  // namespace readyup
