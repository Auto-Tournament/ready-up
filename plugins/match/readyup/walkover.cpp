#include "readyup/walkover.h"
#include "readyup/text.h"

#include "readyup/engine.h"
#include "readyup/match_signals.h"
#include "readyup/walkover_logic.h"

#include <chrono>

namespace readyup {
namespace {

long long NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

WebhookTeam TeamOf(const WebhookMatchContext& ctx, uint64_t steamid64) {
  const auto it = ctx.roster_team.find(steamid64);
  return it == ctx.roster_team.end() ? WebhookTeam::Unknown : it->second;
}

bool Open(const WebhookMatchContext& ctx) {
  return ctx.walkover_short != WebhookTeam::Unknown && ctx.walkover_expires_ms > NowMs();
}

void Tell(int slot, const char* key) {
  if (slot >= 0) text::ChatSlot(slot, key);
  else text::ChatAll(key);
}

}  // namespace

void WalkoverCommand(uint64_t steamid64, const std::string& playerName, bool take, int slot) {
  const auto ctx = WebhookGetMatchContext();
  if (!ctx || !Open(*ctx)) return Tell(slot, "walkover.none");
  if (!ctx->walkover_choosers.count(steamid64)) {
    return Tell(slot, "walkover.not_chooser");
  }
  signals::Emit("walkover_choice", WalkoverChoiceData(steamid64, playerName, take));
}

std::string WalkoverHudSpec(const WebhookMatchContext& ctx, uint64_t viewer, const std::string& lang) {
  if (!Open(ctx)) return {};
  const WebhookTeam shortTeam = ctx.walkover_short;
  const WebhookTeam mine = TeamOf(ctx, viewer);
  WalkoverView v;
  v.shortName = shortTeam == WebhookTeam::Team1 ? ctx.team1_name : ctx.team2_name;
  v.fullName = shortTeam == WebhookTeam::Team1 ? ctx.team2_name : ctx.team1_name;
  v.secondsLeft = SecondsUntil(ctx.walkover_expires_ms, NowMs());
  v.onShortTeam = mine == shortTeam;
  v.onFullTeam = mine != WebhookTeam::Unknown && mine != shortTeam;
  v.chooser = ctx.walkover_choosers.count(viewer) != 0;
  return WalkoverSpec(v, lang);
}

long long FullTeamSecondsLeft(const WebhookMatchContext& ctx) {
  const long long now = NowMs();
  if (ctx.full_team_deadline_ms <= now) return -1;
  return SecondsUntil(ctx.full_team_deadline_ms, now);
}

}  // namespace readyup
