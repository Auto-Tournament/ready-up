// Coaches (see coach.h; the policy is coach_rules.h).
#include "readyup/coach.h"

#include "readyup/coach_rules.h"
#include "readyup/engine.h"
#include "readyup/esports.h"
#include "readyup/host.h"
#include "readyup/logging.h"
#include "readyup/match_events.h"
#include "readyup/match_state.h"
#include "readyup/modes.h"
#include "readyup/players.h"
#include "readyup/ruleset.h"
#include "readyup/webhook.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace readyup {
namespace {

struct Coach {
  int team = 0;             // 1 / 2
  bool listed = false;      // came from the match config (not typed / assigned)
  std::string name;
  bool effective = false;   // m_iCoachingTeam is set and the player was told
  int hintedTeam = -1;      // engine team the "join Spectators" hint was sent for
};

std::mutex g_mu;
std::unordered_map<uint64_t, Coach> g_coaches;
std::unordered_set<uint64_t> g_optOut;         // listed coaches who typed .uncoach (this match)
std::unordered_map<uint64_t, int> g_written;   // SteamID64 -> slot whose m_iCoachingTeam Ready Up set
uint64_t g_ctxMatch = 0;                       // the match context the coaches belong to (0 = none)
bool g_cvarOn = false;                         // sv_coaching_enabled 1 was sent
bool g_cvarDirty = true;                       // map start: send it again
double g_nextSync = 0;

// Schema offsets (-2 = not looked up yet, -1 = missing).
int g_offTeam = -2;
int g_offCoach = -2;

constexpr double kSyncEvery = 1.0;

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::vector<std::string> Split(const std::string& s) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    const size_t b = i;
    while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    if (i > b) out.push_back(s.substr(b, i - b));
  }
  return out;
}

void Private(int slot, const std::string& msg) {
  if (slot < 0 || !ClientPrintChat(slot, (" \x04[ReadyUp]\x01 " + msg).c_str())) SendToChat(("Ready Up: " + msg).c_str());
}

// ---- engine (game thread) ---------------------------------------------------------------------

bool ResolveOffsets() {
  const ru_api* a = host::Api();
  if (!a || a->entity_system_status(a->self) != RU_ENTSYS_OK) return false;
  if (g_offTeam == -2 || g_offCoach == -2) {
    g_offTeam = a->schema_offset(a->self, "CCSPlayerController", "m_iTeamNum");
    if (g_offTeam < 0) g_offTeam = a->schema_offset(a->self, "CBaseEntity", "m_iTeamNum");
    g_offCoach = a->schema_offset(a->self, "CCSPlayerController", "m_iCoachingTeam");
    if (g_offTeam < 0 || g_offCoach < 0) {
      Print("coach: schema fields missing (m_iTeamNum=%d m_iCoachingTeam=%d); coaching is unavailable\n", g_offTeam,
            g_offCoach);
    }
  }
  return g_offTeam >= 0 && g_offCoach >= 0;
}

bool CoachingFieldAvailable() { return ResolveOffsets(); }

void* ControllerOf(int slot) {
  const ru_api* a = host::Api();
  if (!a || slot < 0 || slot >= 64) return nullptr;
  void* ent = a->entity_by_index(a->self, slot + 1);
  if (!ent) return nullptr;
  const char* cls = a->entity_classname(a->self, ent);
  return cls && std::strcmp(cls, "cs_player_controller") == 0 ? ent : nullptr;
}

int ReadTeam(void* ctrl) {
  uint8_t v = 0;
  std::memcpy(&v, static_cast<unsigned char*>(ctrl) + g_offTeam, sizeof(v));
  return v;
}

int ReadCoaching(void* ctrl) {
  int32_t v = 0;
  std::memcpy(&v, static_cast<unsigned char*>(ctrl) + g_offCoach, sizeof(v));
  return v;
}

void WriteCoaching(void* ctrl, int value) {
  const int32_t v = value;
  std::memcpy(static_cast<unsigned char*>(ctrl) + g_offCoach, &v, sizeof(v));
  if (const ru_api* a = host::Api()) a->entity_mark_changed(a->self, ctrl);
}

std::optional<int> SlotOf(uint64_t steamid64) {
  const ru_api* a = host::Api();
  if (!a || !steamid64) return std::nullopt;
  const int s = a->slot_for_steamid(a->self, steamid64);
  return s >= 0 ? std::optional<int>(s) : std::nullopt;
}

// Engine team of a connected player (0 unassigned, 1 spec, 2 T, 3 CT), -1 if unknown.
int EngineTeamOf(uint64_t steamid64) {
  if (!ResolveOffsets()) return -1;
  const auto slot = SlotOf(steamid64);
  if (!slot) return -1;
  void* c = ControllerOf(*slot);
  return c ? ReadTeam(c) : -1;
}

// ---- match context ------------------------------------------------------------------------------

struct Ctx {
  std::optional<WebhookMatchContext> match;
  bool possible = false;  // a match / scrim is loaded, or scrim warmup
  bool scrim = false;
  bool admitted = true;
  int limit = kDefaultCoachesPerTeam;
  std::string team1Name, team2Name;
};

Ctx ReadCtx() {
  Ctx c;
  c.match = WebhookGetMatchContext();
  const ReadyUpMode mode = GetMode();
  if (c.match) {
    c.possible = mode != ReadyUpMode::Practice && mode != ReadyUpMode::External;
    c.scrim = c.match->slug == "scrim";
    c.limit = c.match->coaches_per_team;
    c.team1Name = c.match->team1_name;
    c.team2Name = c.match->team2_name;
  } else {
    c.possible = mode == ReadyUpMode::ScrimWarmup;
    c.scrim = true;
  }
  c.admitted = CoachesAdmitted(CurrentEffectiveRules());
  return c;
}

int ListedTeam(const Ctx& c, uint64_t sid, bool* listed) {
  *listed = false;
  if (!c.match) return 0;
  if (auto it = c.match->coach_team.find(sid); it != c.match->coach_team.end()) {
    *listed = true;
    return static_cast<int>(it->second);
  }
  if (c.match->coaches.count(sid)) *listed = true;
  return 0;
}

int RosterTeam(const Ctx& c, uint64_t sid) {
  if (!c.match) return 0;
  auto it = c.match->roster_team.find(sid);
  return it == c.match->roster_team.end() ? 0 : static_cast<int>(it->second);
}

// team1's side at the start of this half: the map's start side (knife pick included) and the
// halves played. No match: team1 = CT (scrim warmup: CT becomes team1 when it goes live).
bool FallbackTeam1IsCt(const Ctx& c) {
  if (!c.match) return true;
  const int map = std::max(1, MatchStateGet().map_number);
  bool ct = !(static_cast<size_t>(map) <= c.match->map_sides.size() &&
              c.match->map_sides[static_cast<size_t>(map - 1)] == "team2_ct");
  if (MatchEventsSnapshot().swapCount % 2 != 0) ct = !ct;
  return ct;
}

SideTally Tally(const Ctx& c) {
  SideTally t;
  if (!c.match || !ResolveOffsets()) return t;
  for (const auto& kv : c.match->roster_team) {
    const int team = EngineTeamOf(kv.first);
    if (team != 2 && team != 3) continue;
    if (kv.second == WebhookTeam::Team1) (team == 3 ? t.team1_ct : t.team1_t)++;
    else if (kv.second == WebhookTeam::Team2) (team == 3 ? t.team2_ct : t.team2_t)++;
  }
  return t;
}

std::string SideName(int side) { return side == 3 ? "CT" : side == 2 ? "T" : "?"; }

// A new match context (or none): coaches typed or assigned for the previous one go. Scrim warmup
// coaches stay when that scrim goes live.
void OnContextLocked(const Ctx& c) {
  const uint64_t id = c.match ? c.match->matchid : 0;
  if (id == g_ctxMatch) return;
  const bool warmupToScrim = g_ctxMatch == 0 && c.match && c.scrim;
  if (!warmupToScrim) {
    g_coaches.clear();
    g_optOut.clear();
  }
  g_ctxMatch = id;
}

// Actions collected under the lock, run after it (chat and commands go through other modules).
struct Out {
  std::vector<std::pair<int, std::string>> priv;  // slot, message
  std::vector<std::string> all;
  std::vector<std::string> cmds;
  void Flush() {
    for (const auto& p : priv) Private(p.first, p.second);
    for (const auto& m : all) SendToChat(("Ready Up: " + m).c_str());
    for (const auto& cmd : cmds) (void)EnqueueServerCommand(cmd.c_str());
  }
};

// Clears m_iCoachingTeam on a connected player Ready Up set it on.
void ClearFieldLocked(uint64_t sid) {
  auto it = g_written.find(sid);
  if (it == g_written.end()) return;
  const auto slot = SlotOf(sid);
  if (slot && ResolveOffsets()) {
    if (void* ctrl = ControllerOf(*slot); ctrl && ReadCoaching(ctrl) != 0) WriteCoaching(ctrl, 0);
  }
  g_written.erase(it);
}

void SyncLocked(const Ctx& c, Out* out) {
  OnContextLocked(c);
  const bool usable = c.possible && c.admitted && ResolveOffsets();

  if (c.match) {
    // A coach who became a rostered player (a scrim that went live with them on CT/T, a fleet
    // roster update) plays instead.
    std::vector<uint64_t> playing;
    for (const auto& kv : g_coaches) {
      if (RosterTeam(c, kv.first) != 0) playing.push_back(kv.first);
    }
    for (uint64_t sid : playing) {
      if (const auto slot = SlotOf(sid)) out->priv.emplace_back(*slot, "you are on a team's roster now: coaching stopped.");
      ClearFieldLocked(sid);
      g_coaches.erase(sid);
    }
  }
  if (usable && c.match) {
    // Listed coaches with a team coach it without typing anything (until they .uncoach).
    for (const auto& kv : c.match->coach_team) {
      if (g_coaches.count(kv.first) || g_optOut.count(kv.first)) continue;
      if (RosterTeam(c, kv.first) != 0) continue;
      Coach e;
      e.team = static_cast<int>(kv.second);
      e.listed = true;
      for (const auto& h : ListHumans()) {
        if (h.steamid64 == kv.first) e.name = h.name;
      }
      g_coaches[kv.first] = std::move(e);
    }
  }
  if (!usable) {
    if (!c.possible || !c.admitted) {
      for (const auto& kv : g_coaches) ClearFieldLocked(kv.first);
      g_coaches.clear();
    }
  }

  // sv_coaching_enabled: the engine only honours m_iCoachingTeam while it is 1.
  const bool wantCvar = usable && !g_coaches.empty();
  if (wantCvar != g_cvarOn || (wantCvar && g_cvarDirty)) {
    out->cmds.push_back(wantCvar ? "sv_coaching_enabled 1" : "sv_coaching_enabled 0");
    g_cvarOn = wantCvar;
  }
  g_cvarDirty = false;

  if (usable && !g_coaches.empty()) {
    const SideTally t = Tally(c);
    const bool fallback = FallbackTeam1IsCt(c);
    for (auto& kv : g_coaches) {
      Coach& e = kv.second;
      const auto slot = SlotOf(kv.first);
      void* ctrl = slot ? ControllerOf(*slot) : nullptr;
      if (!ctrl) {
        e.effective = false;
        e.hintedTeam = -1;
        continue;
      }
      if (e.name.empty()) {
        for (const auto& h : ListHumans()) {
          if (h.steamid64 == kv.first) e.name = h.name;
        }
      }
      const int engineTeam = ReadTeam(ctrl);
      const std::string label = CoachTeamLabel(e.team, c.team1Name, c.team2Name);
      if (engineTeam == 1) {
        const int cur = ReadCoaching(ctrl);
        const int side = CoachingSide(e.team, t, cur, fallback);
        if (side != cur) {
          WriteCoaching(ctrl, side);
          Debug("coach: %llu m_iCoachingTeam %d -> %d (%s)\n", static_cast<unsigned long long>(kv.first), cur, side,
                label.c_str());
        }
        g_written[kv.first] = *slot;
        if (!e.effective) {
          e.effective = true;
          out->priv.emplace_back(*slot, "you coach " + label + " (" + SideName(side) +
                                            "): you watch and talk to your team only. .uncoach to stop.");
        }
        e.hintedTeam = 1;
      } else {
        e.effective = false;
        if ((engineTeam == 2 || engineTeam == 3) && e.hintedTeam != engineTeam) {
          e.hintedTeam = engineTeam;
          out->priv.emplace_back(*slot, "you are " + label +
                                            "'s coach: join Spectators to coach (a coach never takes a player slot).");
        }
      }
    }
  }

  // Anyone Ready Up set the field on who no longer coaches.
  std::vector<uint64_t> stale;
  for (const auto& kv : g_written) {
    if (!g_coaches.count(kv.first)) stale.push_back(kv.first);
  }
  for (uint64_t sid : stale) ClearFieldLocked(sid);
}

void SyncNow() {
  const Ctx c = ReadCtx();
  Out out;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    SyncLocked(c, &out);
  }
  out.Flush();
}

// Applies a decision (game thread, lock held).
void StartCoachingLocked(uint64_t sid, const std::string& name, int team, bool listed) {
  Coach e;
  e.team = team;
  e.listed = listed;
  e.name = name;
  g_coaches[sid] = std::move(e);
  g_optOut.erase(sid);
}

CoachRequest BuildRequestLocked(const Ctx& c, uint64_t sid, CoachArg arg, bool byAdmin) {
  CoachRequest r;
  r.coaching_possible = c.possible;
  r.scrim = c.scrim;
  r.admitted = c.admitted;
  r.by_admin = byAdmin;
  r.roster_team = RosterTeam(c, sid);
  r.listed_team = ListedTeam(c, sid, &r.listed);
  if (auto it = g_coaches.find(sid); it != g_coaches.end()) r.current_team = it->second.team;
  r.arg = arg;
  r.team1_is_ct = Team1IsCtFromTally(Tally(c), FallbackTeam1IsCt(c));
  for (const auto& kv : g_coaches) {
    if (kv.first == sid || kv.second.listed) continue;  // listed coaches are not counted
    (kv.second.team == 1 ? r.coaches_team1 : r.coaches_team2)++;
  }
  r.per_team_limit = c.limit;
  return r;
}

std::vector<CoachCandidate> Candidates() {
  std::vector<CoachCandidate> out;
  for (const auto& h : ListHumans()) {
    if (!h.steamid64) continue;
    out.push_back({h.steamid64, h.userid, h.name});
  }
  return out;
}

std::string NameOf(uint64_t sid) {
  for (const auto& h : ListHumans()) {
    if (h.steamid64 == sid) return h.name;
  }
  return std::to_string(sid);
}

}  // namespace

void CoachChatCommand(uint64_t steamid64, const std::string& playerName, const std::string& text, int slot) {
  if (!steamid64) return;
  const auto parts = Split(text);
  if (parts.empty()) return;
  const std::string cmd = Lower(parts[0]);
  if (slot < 0) {
    if (auto s = SlotOf(steamid64)) slot = *s;
  }
  const Ctx c = ReadCtx();
  Out out;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    OnContextLocked(c);
    if (cmd == ".uncoach") {
      auto it = g_coaches.find(steamid64);
      if (it == g_coaches.end()) {
        out.priv.emplace_back(slot, "you are not coaching.");
      } else {
        const std::string label = CoachTeamLabel(it->second.team, c.team1Name, c.team2Name);
        bool listed = false;
        (void)ListedTeam(c, steamid64, &listed);
        if (listed) g_optOut.insert(steamid64);  // no auto-coaching again this match
        g_coaches.erase(it);
        ClearFieldLocked(steamid64);
        out.all.push_back(playerName + " stopped coaching " + label + ".");
      }
    } else {
      if (!CoachingFieldAvailable()) {
        out.priv.emplace_back(slot, "coaching is unavailable on this server (see the server log).");
      } else {
        const CoachArg arg = ParseCoachArg(parts.size() > 1 ? parts[1] : std::string());
        const CoachRequest r = BuildRequestLocked(c, steamid64, arg, /*byAdmin=*/false);
        const CoachDecision d = DecideCoach(r, c.team1Name, c.team2Name);
        const std::string label = CoachTeamLabel(d.team, c.team1Name, c.team2Name);
        if (!d.ok) {
          out.priv.emplace_back(slot, d.reason + ".");
        } else if (d.already) {
          out.priv.emplace_back(slot, "you already coach " + label + ".");
        } else {
          StartCoachingLocked(steamid64, playerName, d.team, r.listed);
          out.all.push_back(playerName + " is now coaching " + label + ".");
        }
      }
    }
  }
  out.Flush();
  g_nextSync = 0;  // apply on this tick
}

std::string CoachAdminCommand(const std::string& sub, const std::vector<std::string>& args, std::string* announce) {
  if (announce) announce->clear();
  const bool coach = sub == "coach";
  if (args.empty() || (coach && args.size() < 2)) {
    return coach ? "usage: ru match coach <player> team1|team2|ct|t" : "usage: ru match uncoach <player>";
  }
  std::string err;
  const uint64_t sid = ResolveCoachPlayer(args[0], Candidates(), &err);
  if (!sid) return err + ".";
  const std::string name = NameOf(sid);
  const Ctx c = ReadCtx();
  Out out;
  std::string reply;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    OnContextLocked(c);
    if (!coach) {
      auto it = g_coaches.find(sid);
      if (it == g_coaches.end()) {
        reply = name + " is not coaching.";
      } else {
        const std::string label = CoachTeamLabel(it->second.team, c.team1Name, c.team2Name);
        bool listed = false;
        (void)ListedTeam(c, sid, &listed);
        if (listed) g_optOut.insert(sid);
        g_coaches.erase(it);
        ClearFieldLocked(sid);
        reply = name + " no longer coaches " + label + ".";
        if (announce) *announce = name + " stopped coaching " + label + " (admin).";
      }
    } else if (!CoachingFieldAvailable()) {
      reply = "coaching is unavailable on this server (m_iCoachingTeam not found; see the server log).";
    } else {
      const CoachRequest r = BuildRequestLocked(c, sid, ParseCoachArg(args[1]), /*byAdmin=*/true);
      const CoachDecision d = DecideCoach(r, c.team1Name, c.team2Name);
      const std::string label = CoachTeamLabel(d.team, c.team1Name, c.team2Name);
      if (!d.ok) {
        reply = d.reason + ".";
      } else if (d.already) {
        reply = name + " already coaches " + label + ".";
      } else {
        StartCoachingLocked(sid, name, d.team, r.listed);
        reply = name + " now coaches " + label + ".";
        if (announce) *announce = name + " is now coaching " + label + " (admin).";
      }
    }
  }
  out.Flush();
  g_nextSync = 0;
  return reply;
}

void CoachTick(double now) {
  if (now < g_nextSync) return;
  g_nextSync = now + kSyncEvery;
  SyncNow();
}

void CoachOnEvent(const ru_event* e) {
  if (!e) return;
  if (e->type == RU_EVENT_MAP_START) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_cvarDirty = true;
    g_written.clear();  // new controllers
    for (auto& kv : g_coaches) {
      kv.second.effective = false;
      kv.second.hintedTeam = -1;
    }
    g_nextSync = 0;
  } else if (e->type == RU_EVENT_PLAYER_DISCONNECT && e->steamid64) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_written.erase(e->steamid64);
    if (auto it = g_coaches.find(e->steamid64); it != g_coaches.end()) {
      it->second.effective = false;  // told again after a reconnect
      it->second.hintedTeam = -1;
    }
  } else if (e->type == RU_EVENT_PLAYER_TEAM && e->steamid64) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_coaches.count(e->steamid64)) g_nextSync = 0;  // a coach went to / left Spectators
  }
}

bool CoachAllowedOnServer(uint64_t steamid64) {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_coaches.count(steamid64) != 0;
}

std::vector<std::string> CoachStateLines() {
  std::vector<std::string> out;
  const auto ctx = WebhookGetMatchContext();
  std::lock_guard<std::mutex> lk(g_mu);
  std::string line = "coaches:";
  if (g_coaches.empty()) line += " none";
  bool first = true;
  for (const auto& kv : g_coaches) {
    line += first ? " " : ", ";
    first = false;
    line += (kv.second.name.empty() ? std::to_string(kv.first) : kv.second.name) + " (" +
            CoachTeamLabel(kv.second.team, ctx ? ctx->team1_name : std::string(), ctx ? ctx->team2_name : std::string()) +
            (kv.second.effective ? "" : ", not spectating") + (kv.second.listed ? ", listed" : "") + ")";
  }
  line += std::string(" sv_coaching_enabled=") + (g_cvarOn ? "1" : "0");
  out.push_back(line);
  return out;
}

}  // namespace readyup
