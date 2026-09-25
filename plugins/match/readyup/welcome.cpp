#include "readyup/welcome.h"

#include "readyup/engine.h"
#include "readyup/config.h"
#include "readyup/logging.h"
#include "readyup/plugin_api.h"
#include "readyup/modes.h"
#include "readyup/ready_hud.h"
#include "readyup/players.h"
#include "readyup/steamid.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace readyup {
namespace {

using Clock = std::chrono::steady_clock;

// Display timing. The card waits for the player's first spawn after joining a team
// (before that the client is still in the team menu / spawn transition, which wipes
// the center panel), then is re-sent every second with a 2s duration so it stays up
// continuously for ~5 seconds; then the ready HUD (ready_hud.cpp) takes the panel
// over (WelcomeActiveForSteam turns false at kHandOver).
constexpr auto kAfterSpawn = std::chrono::milliseconds(750);       // let the spawn fade finish
constexpr auto kSpawnWait = std::chrono::seconds(20);              // no spawn seen: show anyway
// After a round (re)start: readyup.cfg welcome_round_delay_ms (CS2's "Match started" announcement).
constexpr auto kDelayTentative = std::chrono::milliseconds(1500);  // give log/event a chance to confirm
// How long the card stays: readyup.cfg welcome_show_seconds (default 8). The last send is a
// second before the end (each send lasts 2 s); the HUD takes the panel over at the end.
std::chrono::milliseconds ShowFor() { return std::chrono::milliseconds(std::max(1, Cfg().welcome_show_seconds) * 1000 - 1000); }
std::chrono::milliseconds HandOver() { return std::chrono::milliseconds(std::max(1, Cfg().welcome_show_seconds) * 1000); }
constexpr auto kResendEvery = std::chrono::milliseconds(1000);
constexpr int kEventDurationSeconds = 2;

struct SlotState {
  uint64_t steamid64 = 0;
  std::string name;
  int team = 0;
  bool shown = false;      // already queued/shown for this player on this map
  bool active = false;     // currently being displayed (or waiting to start)
  bool tentative = false;  // team came only from a `jointeam` request so far
  bool waitingSpawn = false;  // queued; the card starts at the player's next spawn
  int respawnRestarts = 0;    // a respawn while the card was up (round restart) started it again
  Clock::time_point startAt{};
  Clock::time_point nextSend{};
  int sent = 0;
};

std::mutex g_mu;
// When the panel is free again after the last round (re)start; a card starting on a spawn waits
// until then too (the spawn usually comes right after the restart).
Clock::time_point g_lastRoundStart{};
std::unordered_map<int, SlotState> g_slots;
std::unordered_set<uint64_t> g_shownSteam;  // once-per-map by SteamID64 (survives reconnect)
std::string g_map;                           // map the state above belongs to

// Set once any confirmed source (team-switch log line, incl. bots, or player_team)
// has been observed. When confirmations are known to flow, a `jointeam` request
// that never got confirmed is treated as rejected and dropped instead of shown.
std::atomic<bool> g_confirmedSourceSeen{false};

static bool WelcomeEnabled() {
  const char* v = std::getenv("READYUP_WELCOME");
  if (v && *v) return !(v[0] == '0' || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F');
  return Cfg().welcome;
}

static const char* SourceName(WelcomeSource s) {
  switch (s) {
    case WelcomeSource::LogLine: return "log";
    case WelcomeSource::ClientCommand: return "jointeam";
    case WelcomeSource::GameEvent: return "player_team";
  }
  return "?";
}

static void ResetLocked(const std::string& map) {
  g_slots.clear();
  g_shownSteam.clear();
  g_map = map;
}

static std::string HtmlEscape(const std::string& in, size_t maxBytes) {
  std::string s = in;
  if (s.size() > maxBytes) {
    size_t cut = maxBytes;
    // Don't split a UTF-8 sequence.
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    s.resize(cut);
    s += "...";
  }
  std::string out;
  out.reserve(s.size() + 16);
  for (char c : s) {
    switch (c) {
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '&': out += "&amp;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default:
        if (static_cast<unsigned char>(c) >= 0x20) out += c;  // drop control bytes (chat colors)
        break;
    }
  }
  return out;
}

static const char* ModeLabel(ReadyUpMode m) {
  switch (m) {
    case ReadyUpMode::Idle: return "Idle";
    case ReadyUpMode::Practice: return "Practice";
    case ReadyUpMode::MatchWarmup: return "Match warmup";
    case ReadyUpMode::MatchKnife: return "Knife round";
    case ReadyUpMode::MatchLive: return "Match live";
    case ReadyUpMode::Postgame: return "Postgame";
    case ReadyUpMode::ScrimWarmup: return "Scrim warmup";
  }
  return "Unknown";
}

static std::string BuildHtml(const std::string& name, int team, ReadyUpMode mode) {
  const bool ct = (team == 3);
  const char* teamColor = ct ? "#5EA8FF" : "#FF9D3B";
  const char* teamName = ct ? "Counter-Terrorists" : "Terrorists";
  const char* hint = (mode == ReadyUpMode::MatchWarmup || mode == ReadyUpMode::ScrimWarmup) ? "type <b>.ready</b> in chat when you are ready"
                                                        : "type <b>.help</b> in chat for commands";

  std::string h;
  h.reserve(640);
  const std::string brand = HudBrandHtml(/*imgHeight=*/32, "fontSize-l");
  if (!brand.empty()) h += brand + "<br>";
  h += "<font class='fontSize-s' color='#5B6068'>build ";
  h += HtmlEscape(BuildVersion(), 40);
  h += "</font><br>";
  h += "<font class='fontSize-m' color='#FFFFFF'>Welcome, ";
  h += HtmlEscape(name.empty() ? std::string("player") : name, 32);
  h += "</font><br>";
  h += "<font class='fontSize-m' color='";
  h += teamColor;
  h += "'>";
  h += teamName;
  h += "</font><font class='fontSize-m' color='#8A8F98'> &#183; ";
  h += ModeLabel(mode);
  h += "</font><br>";
  h += "<font class='fontSize-sm' color='#A3E635'>";
  h += hint;
  h += "</font>";
  return h;
}

// Parses a trailing `<a><b>[<c>]` group list from a log header like
// `Name<2><[U:1:123]>` or `Name<2><[U:1:123]><CT>`.
static bool ParseLogHeader(const std::string& header, std::string& name, int& slot, std::string& idGroup) {
  std::vector<std::string> groups;
  size_t end = header.size();
  while (groups.size() < 3 && end > 0 && header[end - 1] == '>') {
    const size_t lt = header.rfind('<', end - 1);
    if (lt == std::string::npos) break;
    groups.insert(groups.begin(), header.substr(lt + 1, end - 1 - (lt + 1)));
    end = lt;
  }
  if (groups.size() < 2) return false;
  name = header.substr(0, end);
  const std::string& slotStr = groups[0];
  if (slotStr.empty() || slotStr.find_first_not_of("0123456789") != std::string::npos) return false;
  slot = std::atoi(slotStr.c_str());
  idGroup = groups[1];
  return true;
}

}  // namespace

void WelcomeObserveTeamJoin(int slot, int team, uint64_t steamid64, const std::string& name, WelcomeSource src) {
  if (slot < 0 || slot > 63) return;
  if (team != 2 && team != 3) return;
  if (!WelcomeEnabled()) return;

  const bool confirmed = (src != WelcomeSource::ClientCommand);
  if (confirmed) g_confirmedSourceSeen.store(true, std::memory_order_relaxed);
  const auto now = Clock::now();

  std::lock_guard<std::mutex> lk(g_mu);

  auto& s = g_slots[slot];
  // A different player now occupies this slot: start fresh.
  if (s.steamid64 != 0 && steamid64 != 0 && s.steamid64 != steamid64) s = SlotState{};
  if (steamid64 != 0) s.steamid64 = steamid64;
  if (!name.empty()) s.name = name;

  if (s.active) {
    // Already queued/showing: a confirmed source wins over a tentative request.
    if (confirmed || s.tentative) {
      if (s.team != team) {
        s.team = team;
        s.nextSend = std::min(s.nextSend, now);  // refresh the team line promptly
      }
      if (confirmed) s.tentative = false;
    }
    return;
  }

  if (s.shown) return;
  if (s.steamid64 != 0 && g_shownSteam.count(s.steamid64) != 0) {
    s.shown = true;
    return;
  }

  s.shown = true;
  s.active = true;
  s.team = team;
  s.tentative = !confirmed;
  // Shown from the player's first spawn (WelcomeObservePlayerSpawn); if none is seen
  // (a missed event), after kSpawnWait.
  s.waitingSpawn = true;
  s.startAt = now + std::max<Clock::duration>(kSpawnWait, kDelayTentative);
  s.nextSend = s.startAt;
  s.sent = 0;
  if (s.steamid64 != 0) g_shownSteam.insert(s.steamid64);

  if (DebugEnabled()) {
    Debug("welcome: queued slot=%d team=%d steamid64=%llu via %s\n", slot, team,
          static_cast<unsigned long long>(s.steamid64), SourceName(src));
  }
}

void WelcomeObservePlayerSpawn(int slot) {
  if (slot < 0 || slot > 63) return;
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_slots.find(slot);
  if (it == g_slots.end()) return;
  SlotState& s = it->second;
  if (!s.active) return;
  if (!s.waitingSpawn) {
    // The card is up and the player respawned: a round restart (entering scrim warmup ends CS2's
    // warmup and resets the score) wipes the center panel. Start the card again, a few times.
    if (s.respawnRestarts >= 3) return;
    ++s.respawnRestarts;
    s.startAt = std::max(Clock::now() + kAfterSpawn, g_lastRoundStart);
    s.nextSend = s.startAt;
    if (DebugEnabled()) Debug("welcome: slot=%d respawned while the card was up; showing it again\n", slot);
    return;
  }
  s.waitingSpawn = false;
  s.tentative = false;  // spawned on a team: the join went through
  s.startAt = std::max(Clock::now() + kAfterSpawn, g_lastRoundStart);
  s.nextSend = s.startAt;
  if (DebugEnabled()) Debug("welcome: slot=%d spawned, card in %lldms\n", slot, static_cast<long long>(kAfterSpawn.count()));
}

void WelcomeObserveRoundStart() {
  // A round (re)start respawns everyone and clears the center panel: a card that is waiting or
  // showing starts (again) once the round has settled.
  const auto at = Clock::now() + std::chrono::milliseconds(Cfg().welcome_round_delay_ms);
  std::lock_guard<std::mutex> lk(g_mu);
  g_lastRoundStart = at;
  for (auto& kv : g_slots) {
    SlotState& s = kv.second;
    if (!s.active || s.waitingSpawn) continue;
    if (s.respawnRestarts >= 3) continue;
    ++s.respawnRestarts;
    s.startAt = std::max(s.startAt, at);
    s.nextSend = s.startAt;
  }
}

void WelcomeObserveLogLine(const std::string& line) {
  // Fast reject: only team switches and disconnects matter here.
  static const std::string kSwitch = "\" switched from team <";
  static const std::string kDisc = ">\" disconnected";

  const size_t sw = line.find(kSwitch);
  if (sw != std::string::npos) {
    const size_t q = line.find('"');
    if (q == std::string::npos || q >= sw) return;
    std::string name, idGroup;
    int slot = -1;
    if (!ParseLogHeader(line.substr(q + 1, sw - (q + 1)), name, slot, idGroup)) return;
    g_confirmedSourceSeen.store(true, std::memory_order_relaxed);
    if (idGroup == "BOT") return;

    const size_t to = line.find(" to <", sw + kSwitch.size());
    if (to == std::string::npos) return;
    const size_t gt = line.find('>', to + 5);
    if (gt == std::string::npos) return;
    const std::string teamStr = line.substr(to + 5, gt - (to + 5));
    int team = 0;
    if (teamStr == "CT") team = 3;
    else if (teamStr == "TERRORIST") team = 2;
    if (team == 0) return;

    WelcomeObserveTeamJoin(slot, team, ParseSteamId64Loose(idGroup), name, WelcomeSource::LogLine);
    return;
  }

  // Map change (`Loading map "de_x"` / `Started map "de_x"`): our own detection so the
  // once-per-map policy does not depend on the log-file tailer being active.
  if (line.find("Loading map \"") != std::string::npos || line.find("Started map \"") != std::string::npos) {
    const size_t a = line.find("map \"");
    const size_t b = (a == std::string::npos) ? std::string::npos : line.find('"', a + 5);
    if (b != std::string::npos) {
      const std::string map = line.substr(a + 5, b - (a + 5));
      std::lock_guard<std::mutex> lk(g_mu);
      if (!map.empty() && map != g_map) ResetLocked(map);
    }
    return;
  }

  const size_t dc = line.find(kDisc);
  if (dc != std::string::npos) {
    const size_t q = line.find('"');
    if (q == std::string::npos || q >= dc) return;
    std::string name, idGroup;
    int slot = -1;
    if (!ParseLogHeader(line.substr(q + 1, dc + 1 - (q + 1)), name, slot, idGroup)) return;
    std::lock_guard<std::mutex> lk(g_mu);
    // Forget the slot (so the next occupant can get a welcome); the SteamID stays
    // in g_shownSteam so a reconnect on the same map does not re-show it.
    g_slots.erase(slot);
  }
}

void WelcomeTick() {
  if (IsDisabled()) return;

  // Snapshot mode before taking our lock (GetMode takes the modes mutex).
  const ReadyUpMode mode = GetMode();
  const auto now = Clock::now();
  const auto showFor = ShowFor();

  struct Send {
    int slot;
    std::string html;
  };
  std::vector<Send> sends;

  {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto& kv : g_slots) {
      SlotState& s = kv.second;
      if (!s.active) continue;
      if (now < s.startAt) continue;
      s.waitingSpawn = false;  // kSpawnWait ran out without a spawn: show it now
      if (s.tentative && s.sent == 0 && g_confirmedSourceSeen.load(std::memory_order_relaxed)) {
        // `jointeam` was never confirmed by the engine: likely rejected. Re-arm.
        s.active = false;
        s.shown = false;
        if (s.steamid64 != 0) g_shownSteam.erase(s.steamid64);
        if (DebugEnabled()) Debug("welcome: dropped unconfirmed jointeam for slot=%d\n", kv.first);
        continue;
      }
      if (now - s.startAt > showFor) {
        s.active = false;
        continue;
      }
      if (now < s.nextSend) continue;
      std::string name = s.name;
      if (name.empty()) {
        if (auto ident = GetSlotIdentity(kv.first)) name = ident->name;
      }
      sends.push_back(Send{kv.first, BuildHtml(name, s.team, mode)});
      s.nextSend = now + kResendEvery;
    }
  }

  static std::atomic<bool> s_warnedUnavailable{false};
  for (const auto& snd : sends) {
    const bool ok = PrintCenterHtmlToClientOnly(snd.slot, snd.html, kEventDurationSeconds, RU_HTML_PRIO_NOTICE);
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_slots.find(snd.slot);
    if (it == g_slots.end()) continue;
    if (ok) {
      if (it->second.sent++ == 0 && DebugEnabled()) {
        Debug("welcome: showing slot=%d team=%d (%zu bytes html)\n", snd.slot, it->second.team, snd.html.size());
      }
    } else if (!s_warnedUnavailable.exchange(true)) {
      PrintLine("welcome: per-client center HTML unavailable (event manager or LegacyGameEventListener missing); welcome screen not shown.");
    }
  }
}

bool WelcomeActiveFor(int slot, uint64_t steamid64) {
  if (steamid64 == 0 && slot < 0) return false;
  const auto now = Clock::now();
  const auto handOver = HandOver();
  std::lock_guard<std::mutex> lk(g_mu);
  for (const auto& kv : g_slots) {
    const SlotState& s = kv.second;
    // By SteamID, or by slot: a card queued from an event that carried no SteamID yet has 0 there,
    // and must still hold the HUD back.
    const bool same = (steamid64 != 0 && s.steamid64 == steamid64) ||
                      (slot >= 0 && kv.first == slot && (s.steamid64 == 0 || steamid64 == 0 || s.steamid64 == steamid64));
    if (!same) continue;
    // Queued-but-not-yet-sent counts (don't let the banner race in first); a screen
    // that never made it out (center HTML unavailable) does not block the banner.
    if (!s.active && s.sent == 0) continue;
    if (s.active && s.waitingSpawn) return true;  // the card comes at spawn
    // Covers the queue delay and the display window; after kHandOver the ready
    // HUD's next send replaces the card (no gap: the last card send lasts 2s).
    if (now < s.startAt + handOver) return true;
  }
  return false;
}

bool WelcomeActiveForSteam(uint64_t steamid64) { return WelcomeActiveFor(-1, steamid64); }

}  // namespace readyup
