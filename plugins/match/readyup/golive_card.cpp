#include "readyup/golive_card.h"

#include "readyup/admin_call.h"
#include "readyup/card_html.h"
#include "readyup/config.h"
#include "readyup/engine.h"
#include "readyup/golive_card_logic.h"
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
#include <unordered_map>

namespace readyup {
namespace {

using Clock = std::chrono::steady_clock;

// `ru match start` queues `mp_restartgame 1`: its round start comes about a second later. Without
// round_start events (log-driven rounds) the card starts this long after that instead.
constexpr auto kRestartWait = std::chrono::milliseconds(1500);

struct Card {
  bool armed = false;
  bool started = false;
  bool roundStarted = false;  // the go-live round has started: its round_freeze_end ends the card
  std::string reason;
  GoLiveCardClock clock;
  std::string html;  // built once at the first send; the same HTML every frame
  int sends = 0;     // ticks that sent the card (debug summary)
  int64_t firstSendMs = 0;
  std::unordered_map<int, std::string> skipped;  // slot -> why the card is held back (debug)
};

std::mutex g_mu;
Card g_card;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

int64_t RoundDelayMs() { return Cfg().welcome_round_delay_ms; }

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

// Caller holds g_mu.
void DisarmLocked(GoLiveCardStop why, int64_t now) {
  if (DebugEnabled()) {
    if (g_card.started) {
      const int64_t shown = now - g_card.firstSendMs;
      Debug("golive-card: done (%s) after %lldms, %d sends (avg %.1fms apart)\n", GoLiveCardStopName(why),
            static_cast<long long>(shown), g_card.sends,
            g_card.sends > 1 ? static_cast<double>(shown) / (g_card.sends - 1) : 0.0);
    } else {
      Debug("golive-card: dropped before it was shown (%s, %s)\n", GoLiveCardStopName(why), g_card.reason.c_str());
    }
  }
  g_card.armed = false;
  g_card.skipped.clear();
}

}  // namespace

void GoLiveCardArm(const char* reason, bool restartPending) {
  const int seconds = Cfg().golive_card_seconds;
  std::lock_guard<std::mutex> lk(g_mu);
  if (seconds <= 0) {
    if (DebugEnabled()) Debug("golive-card: not armed (%s): golive_card_seconds=0\n", reason ? reason : "");
    g_card = Card{};
    return;
  }
  const int64_t now = NowMs();
  g_card = Card{};
  g_card.armed = true;
  g_card.roundStarted = !restartPending;
  g_card.reason = reason ? reason : "";
  g_card.clock.startMs = now + RoundDelayMs() + (restartPending ? kRestartWait.count() : 0);
  g_card.clock.untilMs = GoLiveCardUntilMs(g_card.clock.startMs, seconds);
  if (DebugEnabled()) {
    Debug("golive-card: armed (%s), starts in %lldms, %ds (resend every %dms)\n", g_card.reason.c_str(),
          static_cast<long long>(g_card.clock.startMs - now), seconds, Cfg().hud_resend_ms);
  }
}

void GoLiveCardObserveRoundStart() {
  const int seconds = Cfg().golive_card_seconds;
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g_card.armed || g_card.started) return;
  g_card.roundStarted = true;
  g_card.clock.freezeEnded = false;  // a freeze end before this round start was the old round's
  const int64_t at = NowMs() + RoundDelayMs();
  if (at <= g_card.clock.startMs) return;
  g_card.clock.startMs = at;
  g_card.clock.untilMs = GoLiveCardUntilMs(at, seconds);
}

void GoLiveCardObserveFreezeEnd() {
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g_card.armed || !g_card.roundStarted) return;
  g_card.clock.freezeEnded = true;  // the next tick ends the card
}

void GoLiveCardTick() {
  int64_t now = NowMs();
  bool started = false;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_card.armed) return;
    // Cheap early out while the start delay runs (nothing can end it early but a freeze end).
    if (now < g_card.clock.startMs && !g_card.clock.freezeEnded) return;
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
    now = NowMs();
    GoLiveCardStop why = GoLiveCardStop::None;
    switch (GoLiveCardDecide(g_card.clock, now, live, hudBusy, Cfg().hud_resend_ms, &why)) {
      case GoLiveCardAction::Wait: return;
      case GoLiveCardAction::Stop: DisarmLocked(why, now); return;
      case GoLiveCardAction::Send: break;
    }
    if (!g_card.started) {
      if (firstHtml.empty()) return;  // re-armed in between: next tick
      g_card.started = true;
      g_card.firstSendMs = now;
      first = true;
      g_card.html = firstHtml;
    }
    g_card.clock.lastSendMs = now;
    ++g_card.sends;
    html = g_card.html;
    reason = g_card.reason;
  }

  int humans = 0;
  int ok = 0;
  std::unordered_map<int, std::string> skipped;
  for (const auto& h : ListHumans()) {
    const int slot = h.slot >= 0 ? h.slot : h.userid;
    if (h.steamid64 == 0 || slot < 0 || slot > 63) continue;
    ++humans;
    // Another card of ours owns this player's panel right now.
    const char* skip = AdminCallCardActiveFor(slot)              ? ".admin call card"
                       : WelcomeActiveFor(slot, h.steamid64)     ? "welcome card"
                       : ReadyHudAnimActive(h.steamid64)         ? ".ru hud anim"
                                                                 : nullptr;
    if (!skip) {
      const int sent = SendCenterHtml(slot, html, Cfg().hud_duration_s, RU_HTML_PRIO_NOTICE);
      if (sent == 1) {
        ++ok;
        continue;
      }
      skip = sent < 0 ? "a higher-priority panel is up" : "center HTML unavailable";
    }
    skipped[slot] = skip;
  }

  if (DebugEnabled()) {
    // Skips are logged when they start and end (not every frame).
    std::lock_guard<std::mutex> lk(g_mu);
    for (const auto& kv : skipped) {
      const auto it = g_card.skipped.find(kv.first);
      if (it == g_card.skipped.end() || it->second != kv.second) {
        Debug("golive-card: skipping slot=%d (%s)\n", kv.first, kv.second.c_str());
      }
    }
    for (const auto& kv : g_card.skipped) {
      if (skipped.count(kv.first) == 0) Debug("golive-card: slot=%d gets the card again (was: %s)\n", kv.first, kv.second.c_str());
    }
    g_card.skipped = skipped;
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
