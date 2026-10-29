// End-of-round damage report (see damage_report.h).
#include "readyup/damage_report.h"

#include "readyup/config.h"
#include "readyup/damage_ledger.h"
#include "readyup/engine.h"
#include "readyup/esports.h"
#include "readyup/host.h"
#include "readyup/hud_iface.h"
#include "readyup/hud_screens.h"
#include "readyup/logging.h"
#include "readyup/match_events.h"
#include "readyup/match_state.h"
#include "readyup/modes.h"
#include "readyup/players.h"

#include <chrono>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace readyup {
namespace {

// Game thread only (engine events and ticks).
DamageLedger g_ledger;
bool g_enemyDamage = false;

struct Outgoing {
  int slot = -1;
  bool human = false;
  std::string name;
  std::vector<std::string> lines;
  bool card = false;  // the table card on the Auto Tournament HUD instead of chat lines
  int total = 0;
  struct Row {
    std::string name, given, taken, hp;
    int dmg = 0, took = 0, health = 0;
  };
  std::vector<Row> rows;
};

// The round's damage as the table card (at_kit.xml #at_card, body b-dmg): one row per opponent,
// given / taken (damage /hits) and their health left. The look the .dev menu showed as the
// second variant (2026-10-27: the line card was "ugly").
constexpr int kCardRows = 5;
constexpr const char* kKitLayout = "panorama/layout/custom_game/at_kit.xml";
constexpr double kCardSeconds = 12.0;

void FillCard(Outgoing& o, const DamageParticipant& self, const std::vector<DamageParticipant>& players,
              const DamageLedger& ledger) {
  for (const auto& p : players) {
    if (p.key == self.key || (p.team != 2 && p.team != 3) || p.team == self.team) continue;
    const auto to = ledger.Dealt(self.key, p.key);
    const auto from = ledger.Dealt(p.key, self.key);
    o.total += to.dmg;
    if (static_cast<int>(o.rows.size()) >= kCardRows) continue;
    Outgoing::Row r;
    r.name = p.name;
    r.given = std::to_string(to.dmg) + " /" + std::to_string(to.hits);
    r.taken = std::to_string(from.dmg) + " /" + std::to_string(from.hits);
    r.dmg = to.dmg;
    r.took = from.dmg;
    r.health = ledger.Health(p.key);
    r.hp = r.health > 0 ? std::to_string(r.health) : "DEAD";
    o.rows.push_back(std::move(r));
  }
  o.card = !o.rows.empty();
}

const ru_hud_v1* KitHud() {
  const ru_api* a = host::Api();
  if (!a) return nullptr;
  const auto* h = static_cast<const ru_hud_v1*>(a->get_interface(a->self, RU_HUD_IFACE_NAME, 1));
  if (!h || !h->available || !h->available() || !h->open || !h->set_text || !h->set_class) return nullptr;
  return h;
}

double NowSeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::unordered_map<int, double> g_cardUntil;  // slot -> when its card goes away

bool ShowCard(const Outgoing& o, int round) {
  const ru_hud_v1* h = KitHud();
  if (!h) return false;
  const int k = h->open(kKitLayout);
  if (k <= 0) return false;
  const int s = o.slot;
  auto txt = [&](const std::string& id, const std::string& v) { h->set_text(k, s, id.c_str(), "text", v.c_str()); };
  auto cls = [&](const std::string& id, const char* c, bool on) { h->set_class(k, s, id.c_str(), c, on ? 1 : 0); };
  txt("at_kind_a", "Round " + std::to_string(round));
  txt("at_text_a", std::to_string(o.total) + " damage");
  for (const char* id : {"at_kind_a", "at_text_a"}) {
    cls(id, "cur", true);
    cls(id, "old", false);
  }
  for (const char* id : {"at_kind_b", "at_text_b"}) {
    cls(id, "cur", false);
    cls(id, "old", true);
  }
  for (const char* c : {"c-live", "c-warn", "c-info", "c-ban", "c-muted", "c-acc"}) cls("at_card", c, false);
  for (const char* b : {"b-rules", "b-line", "b-bar"}) cls("at_card", b, false);
  cls("at_card", "b-dmg", true);
  cls("at_card", "chat", false);
  cls("at_card", "nudge", false);
  for (int i = 0; i < kCardRows; ++i) {
    const std::string r = "at_d" + std::to_string(i);
    const bool used = i < static_cast<int>(o.rows.size());
    // Fewer than five opponents: the spare rows collapse. The published at_kit.css has no hide
    // class for rows, so they borrow the sentence part's (.at-part.hide); .at-drow.gone is the
    // proper one from the next addon publish on.
    cls(r, "gone", !used);
    cls(r, "at-part", !used);
    cls(r, "hide", !used);
    if (!used) continue;
    const auto& row = o.rows[i];
    txt(r + "_name", row.name);
    txt(r + "_given", row.given);
    txt(r + "_taken", row.taken);
    txt(r + "_hp", row.hp);
    cls(r, "hit", row.dmg > 0);
    cls(r, "big", row.dmg >= 100);
    cls(r, "took", row.took > 0);
    cls(r, "dead", row.health == 0);
  }
  cls("at_card", "on", true);
  g_cardUntil[s] = NowSeconds() + kCardSeconds;
  return true;
}

void HideCards(bool all) {
  if (g_cardUntil.empty()) return;
  const ru_hud_v1* h = KitHud();
  const int k = h ? h->open(kKitLayout) : 0;
  const double now = NowSeconds();
  for (auto it = g_cardUntil.begin(); it != g_cardUntil.end();) {
    if (all || now >= it->second) {
      if (k > 0) h->set_class(k, it->first, "at_card", "on", 0);
      it = g_cardUntil.erase(it);
    } else {
      ++it;
    }
  }
}

std::vector<Outgoing> g_out;
bool g_outReady = false;  // the next round started: send g_out
int g_outRound = 0;
// Who was on a team when the round started: someone who joined mid-round gets no report
// for it (it would be all zeros: a scrim player taking a bot's place saw one on joining).
std::unordered_set<int> g_roundStartSlots;
// Everyone on a team at the round start: a player who leaves mid-round still shows in the
// others' reports with the damage done to and by them (2026-10-15, a live test).
std::vector<DamageParticipant> g_roundStartPlayers;
bool g_haveRoundStart = false;

int TeamOf(int slot) {
  if (slot < 0) return 0;
  const auto t = GetCsTeamNumForSlot(slot);
  return t ? *t : 0;
}

struct Collect {
  std::vector<DamageParticipant> players;
  std::vector<bool> human;
};

int CollectPlayer(void* user, const ru_player* p) {
  auto* c = static_cast<Collect*>(user);
  if (!p->connected) return 1;
  // Bots have no engine slot in the registry; their log `<N>` is the slot in CS2.
  const int key = p->slot >= 0 ? p->slot : p->userid;
  if (key < 0) return 1;
  int team = TeamOf(key);
  if (team != 2 && team != 3) team = p->team;
  if (team != 2 && team != 3) return 1;
  c->players.push_back(DamageParticipant{key, p->name, team});
  c->human.push_back(!p->is_bot && p->steamid64 != 0);
  return 1;
}

void BuildReports() {
  const ru_api* a = host::Api();
  if (!a) return;
  Collect c;
  a->for_each_player(a->self, &CollectPlayer, &c);
  // Who left since the round started: no report for them (gone), but they are in everyone else's.
  std::unordered_set<int> here;
  for (const auto& p : c.players) here.insert(p.key);
  std::vector<DamageParticipant> all = c.players;
  for (const auto& p : g_roundStartPlayers) {
    if (!here.count(p.key)) all.push_back(p);
  }
  g_out.clear();
  g_outRound = MatchStateGet().round_number;
  for (size_t i = 0; i < c.players.size(); ++i) {
    Outgoing o;
    o.slot = c.players[i].key;
    o.human = c.human[i] && (!g_haveRoundStart || g_roundStartSlots.count(o.slot) > 0);
    o.name = c.players[i].name;
    o.lines = FormatDamageReport(c.players[i], all, g_ledger);
    if (o.human && HudScreensOn()) FillCard(o, c.players[i], all, g_ledger);
    if (!o.lines.empty()) g_out.push_back(std::move(o));
  }
}

void OnGameEvent(void*, const char* name, const ru_game_event* ev) {
  const ru_api* a = host::Api();
  if (!a || !name || !ev) return;
  if (std::strcmp(name, "round_start") == 0) {
    // Last round's report goes out now, in freeze time: at round end players may still be running
    // for an exit or saving (2026-10-27). A new map or a stopped match drops it.
    if (!g_out.empty()) {
      if (GetMode() == ReadyUpMode::MatchLive) g_outReady = true;
      else g_out.clear();
    }
    g_ledger.Reset();
    g_enemyDamage = false;
    Collect c;
    a->for_each_player(a->self, &CollectPlayer, &c);
    g_roundStartSlots.clear();
    for (const auto& p : c.players) g_roundStartSlots.insert(p.key);
    g_roundStartPlayers = c.players;
    g_haveRoundStart = true;
  } else if (std::strcmp(name, "player_hurt") == 0) {
    const int victim = a->ev_get_player_slot(a->self, ev, "userid");
    const int attacker = a->ev_get_player_slot(a->self, ev, "attacker");
    const int dmg = a->ev_get_int(a->self, ev, "dmg_health", 0);
    g_ledger.OnHurt(attacker, victim, dmg, a->ev_get_int(a->self, ev, "health", 0));
    if (attacker >= 0 && victim >= 0 && attacker != victim && dmg > 0) {
      const int ta = TeamOf(attacker), tv = TeamOf(victim);
      if ((ta == 2 || ta == 3) && (tv == 2 || tv == 3) && ta != tv) g_enemyDamage = true;
    }
  } else if (std::strcmp(name, "player_death") == 0) {
    g_ledger.OnDeath(a->ev_get_player_slot(a->self, ev, "userid"));
  } else if (std::strcmp(name, "round_end") == 0) {
    // Warmup rounds never end (mp_ignore_round_win_conditions 1): one that does means something
    // reset the warmup rules (a game mode cfg, another plugin). Said without debug=1, not every time.
    if (GetMode() == ReadyUpMode::MatchWarmup) {
      static int warmupRoundEnds = 0;
      ++warmupRoundEnds;
      if (warmupRoundEnds <= 3 || warmupRoundEnds % 50 == 0) {
        Print("warmup: a round ended during match warmup (%d so far, reason %d): the warmup rules were "
              "reset by something else (game mode cfg, another plugin)\n",
              warmupRoundEnds, a->ev_get_int(a->self, ev, "reason", -1));
      }
    }
    if (!Cfg().damage_report || GetMode() != ReadyUpMode::MatchLive) return;
    if (!PlayerExtrasAllowed(CurrentEffectiveRules())) return;  // valve ruleset: no report
    BuildReports();
  }
}

}  // namespace

void DamageReportInstall(const ru_api* api) {
  for (const char* e : {"round_start", "round_end", "player_hurt", "player_death"}) {
    if (!api->subscribe_game_event(api->self, e, &OnGameEvent, nullptr)) {
      Print("damage-report: could not subscribe to %s\n", e);
    }
  }
}

void DamageReportTick() {
  HideCards(false);
  if (g_out.empty() || !g_outReady) return;  // built at round end, sent at the next round start
  g_outReady = false;
  int humans = 0, lines = 0;
  for (const auto& o : g_out) {
    lines += static_cast<int>(o.lines.size());
    if (o.human) {
      ++humans;
      if (!(o.card && ShowCard(o, g_outRound))) {  // the card instead of chat lines, through freeze time
        (void)ClientPrintChat(o.slot, " \x04[ReadyUp]\x01 Damage report:");
        for (const auto& l : o.lines) (void)ClientPrintChat(o.slot, (" " + l).c_str());
      }
    }
    for (const auto& l : o.lines) Debug("damage-report: %s: %s\n", o.name.c_str(), l.c_str());
  }
  Print("damage-report: round %d: %zu players, %d lines, sent to %d humans\n", g_outRound, g_out.size(), lines, humans);
  g_out.clear();
}

bool DamageReportEnemyDamageThisRound() { return g_enemyDamage; }

}  // namespace readyup
