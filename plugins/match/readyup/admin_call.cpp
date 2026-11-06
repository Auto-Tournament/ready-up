#include "readyup/admin_call.h"
#include "readyup/host.h"
#include "readyup/i18n_api.h"

#include "readyup/hud_screens.h"
#include "readyup/admin_call_logic.h"
#include "readyup/admin_check.h"
#include "readyup/card_html.h"
#include "readyup/config.h"
#include "readyup/engine.h"
#include "readyup/fleet_bridge.h"
#include "readyup/host.h"
#include "readyup/logging.h"
#include "readyup/match_signals.h"
#include "readyup/match_state.h"
#include "readyup/players.h"
#include "readyup/plugin_api.h"
#include "readyup/text.h"
#include "readyup/webhook.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace readyup {
namespace {

using Clock = std::chrono::steady_clock;

// The admin card: sent at 0..4 s, each send lasting 2 s (about 6 s in total).
constexpr auto kCardSendFor = std::chrono::seconds(4);
constexpr auto kCardHold = std::chrono::seconds(6);
constexpr auto kResendEvery = std::chrono::milliseconds(1000);
constexpr int kSendDurationSeconds = 2;
constexpr const char* kPrivatePrefix = " \x04[ReadyUp]\x01 ";

struct AdminCard {
  std::string name, label, message;  // built at each send in the admin's language then
  Clock::time_point startAt{};
  Clock::time_point nextSend{};
};

std::mutex g_mu;
AdminCallCooldown g_cooldown;
std::unordered_map<int, AdminCard> g_cards;  // by admin slot

void Tell(int slot, const char* key, const text::Args& args = {}) {
  if (slot >= 0) text::ChatSlot(slot, key, args);
  else text::ChatAll(key, args);  // slot unknown: better seen by all than lost
}

std::string NewCallId() {
  static std::mutex mu;
  static std::mt19937_64 rng{[] {
    std::random_device rd;
    return (static_cast<uint64_t>(rd()) << 32) ^ rd() ^
           static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
  }()};
  unsigned char b[16];
  {
    std::lock_guard<std::mutex> lk(mu);
    const uint64_t a = rng(), c = rng();
    for (int i = 0; i < 8; ++i) {
      b[i] = static_cast<unsigned char>(a >> (8 * i));
      b[8 + i] = static_cast<unsigned char>(c >> (8 * i));
    }
  }
  return CallIdFromBytes(b);
}

long long UnixMsNow() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

void AdminCallCommand(uint64_t steamid64, const std::string& playerName, const std::string& text) {
  if (steamid64 == 0) return;

  // Who is calling: slot, side, team.
  HumanIdentity me;
  me.steamid64 = steamid64;
  me.name = playerName;
  for (const auto& h : ListHumans()) {
    if (h.steamid64 == steamid64) {
      me = h;
      break;
    }
  }
  const int slot = me.slot >= 0 ? me.slot : me.userid;

  int left = 0;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_cooldown.TryCall(steamid64, host::NowSeconds(), Cfg().admin_call_cooldown_s, &left)) {
      Tell(slot, "admincall.cooldown", {{"seconds", std::to_string(left)}});
      if (HudScreensOn() && slot >= 0) {
        HudScreensNotice(slot, NoticeSpec("id=notice\nwhere=card\ncolor=muted\n", "notice.admin", "notice.already_called",
                                          "notice.again_in", {{"seconds", std::to_string(left)}}),
                         5.0);
      }
      return;
    }
  }

  // The message: everything after `.admin`.
  std::string rest;
  const size_t sp = text.find_first_of(" \t");
  if (sp != std::string::npos) rest = text.substr(sp + 1);

  const auto ctx = WebhookGetMatchContext();
  const bool match = ctx && ctx->slug != "scrim";  // a scrim's context is not a platform match

  AdminCallEvent e;
  e.matchid = match ? static_cast<long long>(ctx->matchid) : -1;
  e.map_number = std::max(1, MatchStateGet().map_number) - 1;  // 0-based in admin_called
  e.server_id = fleet_bridge::FleetServerId();
  e.call_id = NewCallId();
  e.player.steamid64 = steamid64;
  e.player.name = CleanAdminCallMessage(playerName.empty() ? me.name : playerName, 64);
  e.player.side = me.team == 3 ? "ct" : me.team == 2 ? "t" : "";
  const bool spectator = me.team == 1;
  std::string teamName;
  if (spectator) {
    e.player.team = "spectator";
  } else if (match) {
    const auto it = ctx->roster_team.find(steamid64);
    if (it != ctx->roster_team.end() && it->second == WebhookTeam::Team1) {
      e.player.team = "team1";
      teamName = ctx->team1_name;
    } else if (it != ctx->roster_team.end() && it->second == WebhookTeam::Team2) {
      e.player.team = "team2";
      teamName = ctx->team2_name;
    }
  }
  e.message = CleanAdminCallMessage(rest);
  e.called_at = IsoUtcFromUnixMs(UnixMsNow());

  // The platform: webhook (no-op without an events URL) and the fleet link (while assigned).
  WebhookEmitAdminCalled(e);
  if (signals::Enabled()) signals::Emit("admin_called", AdminCallData(e));

  const std::string label = AdminCallTeamLabel(CleanAdminCallMessage(teamName, 32), e.player.side, spectator);
  const std::string who = e.player.name + (label.empty() ? "" : " (" + label + ")");
  Print("admin-call: %s (%llu%s%s): %s [%s]\n", e.player.name.c_str(), static_cast<unsigned long long>(steamid64),
        label.empty() ? "" : ", ", label.c_str(), e.message.empty() ? "(no message)" : e.message.c_str(),
        e.call_id.c_str());

  // In-game admins: a private chat line and the card.
  int admins = 0;
  const auto now = Clock::now();
  for (const auto& h : ListHumans()) {
    const int s = h.slot >= 0 ? h.slot : h.userid;
    if (h.steamid64 == 0 || s < 0 || s > 63 || !IsReadyUpAdmin(h.steamid64)) continue;
    ++admins;
    const std::string lang = i18n::LanguageOf(host::Api(), s);
    const std::string line = "\x07" + text::ForLang(lang, "admincall.head") + "\x01 " +
                             text::ForLang(lang, e.message.empty() ? "admincall.needs" : "admincall.needs_msg",
                                           {{"who", who}, {"message", e.message}});
    (void)ClientPrintChat(s, (kPrivatePrefix + line).c_str());
    AdminCard card{e.player.name, label, e.message, now, now};
    std::lock_guard<std::mutex> lk(g_mu);
    g_cards[s] = std::move(card);
  }
  if (DebugEnabled()) Debug("admin-call: %d admin(s) in game notified\n", admins);

  Tell(slot, "admincall.notified");
  if (HudScreensOn() && slot >= 0) {
    HudScreensNotice(slot, NoticeSpec("id=notice\nwhere=card\ncolor=warn\n", "notice.admin", "notice.admin_coming"),
                     6.0);
  }
}

void AdminCallTick() {
  const auto now = Clock::now();
  std::vector<std::pair<int, AdminCard>> sends;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_cards.empty()) return;
    for (auto it = g_cards.begin(); it != g_cards.end();) {
      AdminCard& c = it->second;
      if (now - c.startAt >= kCardHold) {
        it = g_cards.erase(it);
        continue;
      }
      if (now >= c.nextSend && now - c.startAt <= kCardSendFor) {
        sends.emplace_back(it->first, c);
        c.nextSend = now + kResendEvery;
      }
      ++it;
    }
  }
  const bool screens = HudScreensOn();
  for (const auto& [slot, c] : sends) {
    if (screens) {
      HudScreensNotice(slot, [c](const std::string& lang) { return AdminCallCardSpec(c.name, c.label, c.message, lang); },
                       kSendDurationSeconds);
    } else {
      const std::string lang = i18n::LanguageOf(host::Api(), slot);
      (void)SendCenterHtml(slot, AdminCallCardHtml(c.name, c.label, c.message, lang), kSendDurationSeconds, RU_HTML_PRIO_ALERT);
    }
  }
}

bool AdminCallCardActiveFor(int slot) {
  std::lock_guard<std::mutex> lk(g_mu);
  const auto it = g_cards.find(slot);
  return it != g_cards.end() && Clock::now() - it->second.startAt < kCardHold;
}

}  // namespace readyup
