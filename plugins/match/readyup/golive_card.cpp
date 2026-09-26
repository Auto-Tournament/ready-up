#include "readyup/golive_card.h"

#include "readyup/admin_call.h"
#include "readyup/card_html.h"
#include "readyup/config.h"
#include "readyup/engine.h"
#include "readyup/logging.h"
#include "readyup/match_events.h"
#include "readyup/match_features.h"
#include "readyup/match_state.h"
#include "readyup/modes.h"
#include "readyup/players.h"
#include "readyup/plugin_api.h"
#include "readyup/ready_hud.h"
#include "readyup/webhook.h"
#include "readyup/welcome.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>

namespace readyup {
namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kResendEvery = std::chrono::milliseconds(1000);
constexpr int kSendDurationSeconds = 2;  // each send lasts 2 s: the card never lapses between sends
// `ru match start` queues `mp_restartgame 1`: its round start comes about a second later. Without
// round_start events (log-driven rounds) the card starts this long after that instead.
constexpr auto kRestartWait = std::chrono::milliseconds(1500);

struct Card {
  bool armed = false;
  bool started = false;
  std::string reason;
  Clock::time_point startAt{};
  Clock::time_point sendUntil{};
  Clock::time_point nextSend{};
  std::string html;
};

std::mutex g_mu;
Card g_card;

std::chrono::milliseconds RoundDelay() { return std::chrono::milliseconds(Cfg().welcome_round_delay_ms); }

GoLiveCardInfo BuildInfo() {
  GoLiveCardInfo info;
  info.adminCall = true;
  const auto ctx = WebhookGetMatchContext();
  if (!ctx) return info;
  info.pauses = FeatureEnabled(Feature::Pauses);
  if (ctx->slug == "scrim") return info;  // scrim teams are just CT and T: no matchup line
  info.team1 = ctx->team1_name;
  info.team2 = ctx->team2_name;
  // Side of team1 from the map side (the knife pick updated it) and the swaps so far.
  const int mapNum = std::max(1, MatchStateGet().map_number);
  bool team1Ct = !(static_cast<size_t>(mapNum) <= ctx->map_sides.size() &&
                   ctx->map_sides[static_cast<size_t>(mapNum - 1)] == "team2_ct");
  if (MatchEventsSnapshot().swapCount % 2 != 0) team1Ct = !team1Ct;
  info.team1Side = team1Ct ? 3 : 2;
  return info;
}

}  // namespace

void GoLiveCardArm(const char* reason, bool restartPending) {
  const int seconds = Cfg().golive_card_seconds;
  std::lock_guard<std::mutex> lk(g_mu);
  if (seconds <= 0) {
    g_card = Card{};
    return;
  }
  g_card = Card{};
  g_card.armed = true;
  g_card.reason = reason ? reason : "";
  g_card.startAt = Clock::now() + RoundDelay() + (restartPending ? kRestartWait : Clock::duration::zero());
  g_card.nextSend = g_card.startAt;
  // Sends at 0, 1, ... seconds-2 s, each lasting 2 s: the card is up for `seconds` in total.
  g_card.sendUntil = g_card.startAt + std::chrono::seconds(std::max(0, seconds - 2));
  if (DebugEnabled()) {
    Debug("golive-card: armed (%s), starts in %lldms, %ds\n", g_card.reason.c_str(),
          static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(g_card.startAt - Clock::now()).count()),
          seconds);
  }
}

void GoLiveCardObserveRoundStart() {
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g_card.armed || g_card.started) return;
  const auto at = Clock::now() + RoundDelay();
  if (at <= g_card.startAt) return;
  const auto shift = at - g_card.startAt;
  g_card.startAt = at;
  g_card.nextSend = at;
  g_card.sendUntil += shift;
}

void GoLiveCardTick() {
  const auto now = Clock::now();
  bool started = false;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_card.armed || now < g_card.startAt || now < g_card.nextSend) return;
    started = g_card.started;
  }

  // Outside the lock: these take other modules' locks (GoLiveCardArm runs under the modes lock).
  const bool live = GetMode() == ReadyUpMode::MatchLive;
  const LiveHudInfo hud = MatchFeaturesHud();
  const bool hudBusy = hud.paused || hud.forfeit;
  GoLiveCardInfo info;
  std::string firstHtml;
  if (!started && live && !hudBusy) {
    info = BuildInfo();
    firstHtml = GoLiveCardHtml(info);
  }

  std::string html;
  std::string reason;
  bool first = false;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_card.armed) return;
    if (!live || hudBusy || (g_card.started && now > g_card.sendUntil)) {
      if (DebugEnabled() && g_card.started) {
        Debug("golive-card: done (%s)\n", !live ? "not live any more" : hudBusy ? "live panel took over" : "time up");
      }
      g_card.armed = false;
      return;
    }
    if (!g_card.started) {
      if (firstHtml.empty()) return;  // re-armed in between: next tick
      g_card.started = true;
      first = true;
      g_card.html = firstHtml;
    }
    g_card.nextSend = now + kResendEvery;
    html = g_card.html;
    reason = g_card.reason;
  }

  int humans = 0;
  int ok = 0;
  for (const auto& h : ListHumans()) {
    const int slot = h.slot >= 0 ? h.slot : h.userid;
    if (h.steamid64 == 0 || slot < 0 || slot > 63) continue;
    ++humans;
    // Another card of ours owns this player's panel right now.
    if (AdminCallCardActiveFor(slot) || WelcomeActiveFor(slot, h.steamid64) || ReadyHudAnimActive(h.steamid64)) continue;
    if (SendCenterHtml(slot, html, kSendDurationSeconds, RU_HTML_PRIO_NOTICE) == 1) ++ok;
  }
  if (!first) return;
  if (DebugEnabled()) {
    Debug("golive-card: showing (%s) to %d/%d players (%zu bytes html)\n", reason.c_str(), ok, humans, html.size());
  }
  if (humans > 0 && ok == 0) {
    // Center HTML unavailable (or every panel taken): the commands go to chat once instead.
    SendToChat(info.pauses
                   ? "Ready Up: .p/.pause/.tech pause | .up/.unpause resume | .tac timeout | .admin [message] call an admin"
                   : "Ready Up: .admin [message] call an admin");
  }
}

}  // namespace readyup
