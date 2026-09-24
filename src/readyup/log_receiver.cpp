#include "readyup/log_receiver.h"

#include "readyup/plugin_api.h"
#include "readyup/plugin_loader.h"

#include "readyup/client_command_hook.h"
#include "readyup/config.h"
#include "readyup/command_buffer_hook.h"
#include "readyup/game_events.h"
#include "readyup/knife_tracker.h"
#include "readyup/logging.h"
#include "readyup/match_state.h"
#include "readyup/modes.h"
#include "readyup/backup_files.h"
#include "readyup/persisted_match_state.h"
#include "readyup/player_registry.h"
#include "readyup/path.h"
#include "readyup/slot_registry.h"
#include "readyup/steamid.h"
#include "readyup/webhook.h"
#include "readyup/welcome.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <string>
#include <thread>

namespace readyup {
namespace {

std::atomic<bool> g_started{false};

// Best-effort match lifecycle tracking from console logs.
int g_mapNumber = 1;
int g_roundNumber = 0;
int g_team1Score = 0;  // best-effort: we map CT->team1, T->team2
int g_team2Score = 0;
std::string g_currentMap;

struct KD {
  std::string name;
  WebhookTeam team = WebhookTeam::Unknown;
  int kills = 0;
  int deaths = 0;
};
std::unordered_map<uint64_t, KD> g_kd;

static std::filesystem::path PreferredConsoleLogPath(const std::string& csgoDir) {
  // Written by `con_logfile "readyup/readyup_console.log"` in readyup_autoexec.cfg.
  return std::filesystem::path(csgoDir) / "readyup" / "readyup_console.log";
}

static std::filesystem::path FindNewestLogFile(const std::filesystem::path& logsDir) {
  std::filesystem::path best;
  std::filesystem::file_time_type bestTime{};
  bool any = false;

  std::error_code ec;
  for (const auto& it : std::filesystem::directory_iterator(logsDir, ec)) {
    if (ec) break;
    if (!it.is_regular_file(ec) || ec) continue;
    const auto p = it.path();
    if (p.extension() != ".log") continue;
    const auto t = it.last_write_time(ec);
    if (ec) continue;
    if (!any || t > bestTime) {
      any = true;
      bestTime = t;
      best = p;
    }
  }
  return best;
}

static std::filesystem::path FindNewestFileRecursive(const std::filesystem::path& root) {
  std::filesystem::path best;
  std::filesystem::file_time_type bestTime{};
  bool any = false;

  std::error_code ec;
  for (const auto& it : std::filesystem::recursive_directory_iterator(root, ec)) {
    if (ec) break;
    if (!it.is_regular_file(ec) || ec) continue;
    const auto p = it.path();
    // Prefer common text log extensions (but don't hard-require them).
    const auto ext = p.extension().string();
    if (!(ext == ".log" || ext == ".txt" || ext == ".rpt" || ext == ".out" || ext.empty())) {
      // skip obviously irrelevant stuff
    }
    const auto t = it.last_write_time(ec);
    if (ec) continue;
    if (!any || t > bestTime) {
      any = true;
      bestTime = t;
      best = p;
    }
  }
  return best;
}

static bool ExtractQuoted(const std::string& s, size_t& i, std::string& out) {
  const size_t q1 = s.find('"', i);
  if (q1 == std::string::npos) return false;
  const size_t q2 = s.find('"', q1 + 1);
  if (q2 == std::string::npos) return false;
  out = s.substr(q1 + 1, q2 - (q1 + 1));
  i = q2 + 1;
  return true;
}

static bool ExtractBetween(const std::string& s, const char* a, const char* b, std::string& out) {
  const size_t p1 = s.find(a);
  if (p1 == std::string::npos) return false;
  const size_t start = p1 + std::strlen(a);
  const size_t p2 = s.find(b, start);
  if (p2 == std::string::npos) return false;
  out = s.substr(start, p2 - start);
  return true;
}

static bool ParseScorePair(const std::string& line, const char* leftKey, const char* rightKey, int& leftOut, int& rightOut) {
  std::string a, b;
  if (!ExtractBetween(line, leftKey, "\"", a)) return false;
  if (!ExtractBetween(line, rightKey, "\"", b)) return false;
  leftOut = std::atoi(a.c_str());
  rightOut = std::atoi(b.c_str());
  return true;
}

static WebhookTeam TeamForSteamid(uint64_t steamid64) {
  auto ctx = WebhookGetMatchContext();
  if (!ctx) return WebhookTeam::Unknown;
  auto it = ctx->roster_team.find(steamid64);
  if (it == ctx->roster_team.end()) return WebhookTeam::Unknown;
  return it->second;
}

static void MaybeEmitMapResultAndReset(const std::string& nextMapName) {
  const bool hadPrev = !g_currentMap.empty();
  if (g_mapNumber < 1) g_mapNumber = 1;
  // Map number is only meaningful for a loaded match. Without one (idle, scrim
  // warmup) stay at 1 so a match loaded later doesn't inherit the server's map
  // change count. With one, prefer the map's position in the match maplist
  // (match load changelevels to map 1, series advance to map N).
  if (auto ctx = WebhookGetMatchContext()) {
    int idx = -1;
    for (size_t k = static_cast<size_t>(g_mapNumber); k < ctx->maplist.size(); ++k) {
      if (ctx->maplist[k] == nextMapName) {
        idx = static_cast<int>(k);
        break;
      }
    }
    if (idx < 0) {
      for (size_t k = 0; k < ctx->maplist.size(); ++k) {
        if (ctx->maplist[k] == nextMapName) {
          idx = static_cast<int>(k);
          break;
        }
      }
    }
    if (idx >= 0) g_mapNumber = idx + 1;
    else if (hadPrev) g_mapNumber += 1;
  } else {
    g_mapNumber = 1;
  }
  g_currentMap = nextMapName;
  g_roundNumber = 0;
  g_team1Score = 0;
  g_team2Score = 0;
  g_kd.clear();
  MatchStateSetMap(g_mapNumber, g_currentMap);
  MatchStateSetRound(g_roundNumber);
  MatchStateSetScore(g_team1Score, g_team2Score);
}

static void ParseHeader(const std::string& header, std::string& nameOut, int& slotOut, uint64_t& steamid64Out) {
  // Format usually: name<userid><steamid><team>
  // We'll pull name (before first '<') and steamid (between second '<' and second '>').
  nameOut.clear();
  slotOut = -1;
  steamid64Out = 0;

  const size_t a = header.find('<');
  if (a == std::string::npos) {
    nameOut = header;
    return;
  }
  nameOut = header.substr(0, a);

  const size_t b = header.find('>', a + 1);
  if (b != std::string::npos) {
    const std::string slotStr = header.substr(a + 1, b - (a + 1));
    slotOut = std::atoi(slotStr.c_str());
  }

  const size_t c = header.find('<', b == std::string::npos ? 0 : b + 1);
  const size_t d = header.find('>', c == std::string::npos ? 0 : c + 1);
  if (c == std::string::npos || d == std::string::npos) return;

  // SteamID is the *second* <> group: name<userid><steamid><team>
  const std::string steam = header.substr(c + 1, d - (c + 1));
  steamid64Out = ParseSteamId64Loose(steam);
}

static std::recursive_mutex g_lifecycleMu;

// Match/round lifecycle derived from server log lines. Shared by the in-process
// logging listener (client_command_hook.cpp, the primary source) and the file
// reader below (fallback only when the in-process listener is not installed).
static void LifecycleImpl(const std::string& line) {
  // Chat lines can contain arbitrary text (e.g. a player typing
  // `World triggered "Round_Start"`); never treat them as world/map events.
  const bool isChat =
      line.find("\" say \"") != std::string::npos || line.find("\" say_team \"") != std::string::npos;

  // Knife round: deaths / `attacked` health for the time-out tiebreak.
  if (!isChat && KnifeTrackerActive()) KnifeTrackerObserveLine(line);

  // Map lifecycle (often not prefixed with a player header).
  // Examples vary, so we keep this heuristic and harmless.
  if (!isChat && (line.find("Loading map \"") != std::string::npos || line.find("Started map \"") != std::string::npos)) {
    std::string map;
    if (ExtractBetween(line, "map \"", "\"", map)) {
      if (!map.empty()) {
        plugins::LifecycleEvent e;
        e.type = RU_EVENT_MAP_START;
        e.source = RU_SOURCE_LOG;
        e.map = map;
        plugins::PostEvent(std::move(e));  // deduped (Loading + Started map)
      }
      if (!map.empty() && map != g_currentMap) {
        MaybeEmitMapResultAndReset(map);
      }
    }
  }

  // CS2's own warmup started (typically when the first human joins a map).
  // ReadyUp emulates warmup; the real one's WARMUP text hides our center HTML.
  if (!isChat && line.find("World triggered \"Warmup_Start\"") != std::string::npos) {
    OnNativeWarmupStarted("log Warmup_Start");
    return;
  }

  // mp_restartgame / warmup end: CS2 logs `World triggered "Match_Start"`
  // right before the first Round_Start of the fresh game. Reset the log-derived
  // round counter + score so round numbers restart at 1 after going live.
  if (!isChat && line.find("World triggered \"Match_Start\"") != std::string::npos) {
    {
      plugins::LifecycleEvent e;
      e.type = RU_EVENT_MATCH_START;
      e.source = RU_SOURCE_LOG;
      plugins::PostEvent(std::move(e));
    }
    if (GameEventsListenerInstalled()) return;
    g_roundNumber = 0;
    g_team1Score = 0;
    g_team2Score = 0;
    g_kd.clear();
    MatchStateSetRound(g_roundNumber);
    MatchStateSetScore(g_team1Score, g_team2Score);
    return;
  }

  // Round start/end (world triggers don't have player header).
  if (!isChat && line.find("World triggered \"Round_Start\"") != std::string::npos) {
    // Knife round (log-driven; an engine round_start may also arrive, deduped
    // by knife phase). Not a match round: no counters, no webhooks.
    if (GetMode() == ReadyUpMode::MatchKnife) {
      KnifeOnRoundStart(g_mapNumber, "log");
      return;
    }
    if (GameEventsListenerInstalled()) {
      // Engine events drive round lifecycle when available.
      return;
    }
    g_roundNumber += 1;
    {
      plugins::LifecycleEvent e;
      e.type = RU_EVENT_ROUND_START;
      e.source = RU_SOURCE_LOG;
      e.round = g_roundNumber;
      e.team_ct_score = g_team1Score;
      e.team_t_score = g_team2Score;
      plugins::PostEvent(std::move(e));
    }
    OnMatchRoundStarted();
    MatchStateSetRound(g_roundNumber);
    WebhookEmitRoundStarted(g_mapNumber, g_roundNumber, g_team1Score, g_team2Score);
    return;
  }

  // Common CS2 end-of-round notices include CT/T scores.
  if (!isChat && line.find("triggered \"SFUI_Notice_") != std::string::npos) {
    // Knife round end, e.g.
    //   Team "TERRORIST" triggered "SFUI_Notice_Terrorists_Win" (CT "0") (T "1")
    //   Team "CT" triggered "SFUI_Notice_Target_Saved" (CT "1") (T "0")
    // Not a match round: no score update, no webhooks, no map-end check.
    if (GetMode() == ReadyUpMode::MatchKnife) {
      std::string team, notice;
      (void)ExtractBetween(line, "Team \"", "\"", team);
      (void)ExtractBetween(line, "triggered \"", "\"", notice);
      const int cs = (team == "CT") ? 3 : (team == "TERRORIST") ? 2 : 0;
      const bool elimination = (notice == "SFUI_Notice_CTs_Win" || notice == "SFUI_Notice_Terrorists_Win");
      KnifeOnRoundEnd(g_mapNumber, cs, elimination, "log", notice);
      return;
    }
    int ct = 0, tt = 0;
    if (ParseScorePair(line, "(CT \"", "(T \"", ct, tt)) {
      g_team1Score = ct;
      g_team2Score = tt;
    }
    MatchStateSetScore(g_team1Score, g_team2Score);
    if (GameEventsListenerInstalled()) {
      // Keep using logs for score updates, but do not emit round_end when
      // engine events are installed.
      return;
    }
    const char* winner = nullptr;
    if (line.find("CTs_Win") != std::string::npos) winner = "team1";
    else if (line.find("Terrorists_Win") != std::string::npos) winner = "team2";
    if (!winner) winner = (g_team1Score >= g_team2Score) ? "team1" : "team2";
    if (g_roundNumber == 0) g_roundNumber = 1;
    {
      plugins::LifecycleEvent e;
      e.type = RU_EVENT_ROUND_END;
      e.source = RU_SOURCE_LOG;
      e.round = g_roundNumber;
      e.winner = line.find("CTs_Win") != std::string::npos ? 3
                 : line.find("Terrorists_Win") != std::string::npos ? 2 : 0;
      e.team_ct_score = g_team1Score;
      e.team_t_score = g_team2Score;
      plugins::PostEvent(std::move(e));
    }
    std::vector<WebhookPlayerStatLine> players;
    players.reserve(g_kd.size());
    for (const auto& kv : g_kd) {
      const uint64_t sid = kv.first;
      const KD& s = kv.second;
      if (sid == 0) continue;
      if (s.team == WebhookTeam::Unknown) continue;
      WebhookPlayerStatLine out;
      out.steamid64 = sid;
      out.name = s.name;
      out.team = s.team;
      out.kills = s.kills;
      out.deaths = s.deaths;
      players.push_back(std::move(out));
    }
    WebhookEmitRoundEndWithPlayerStats(
        g_mapNumber,
        g_roundNumber,
        /*round_time=*/0,
        /*reason=*/0,
        winner,
        g_team1Score,
        g_team2Score,
        players);

    // Persist a minimal snapshot for crash/restart recovery (log-derived path).
    // Only while a match is loaded (idle/scrim-warmup rounds are not match state).
    if (WebhookGetMatchContext()) {
      readyup::persisted_match_state::PersistSnapshot(g_mapNumber, g_roundNumber, g_team1Score, g_team2Score);
    }

    // Backup file discovery (log-derived path; when engine events are unavailable).
    if (auto ctxOpt = WebhookGetMatchContext()) {
      const auto& ctx = *ctxOpt;
      const std::string prefix =
          "readyup_backup_" + std::to_string(static_cast<unsigned long long>(ctx.matchid)) +
          "_map" + std::to_string(g_mapNumber <= 0 ? 1 : g_mapNumber) + "_";
      readyup::persisted_match_state::PersistBackupPrefix(prefix);
      const std::string cmd = "mp_backup_round_file " + prefix;
      (void)EnqueueServerCommand(cmd.c_str());
      readyup::backup_files::DiscoverAndPersistNewestBackupFileAsync(prefix);
    }
    OnMatchRoundEnded(g_mapNumber, g_team1Score, g_team2Score, g_currentMap);
    return;
  }

  // Typical: L 02/07/2026 - 12:00:00: "Name<...><STEAM_...><...>" say "message"
  // We best-effort parse the first quoted header and (optionally) the chat message.
  size_t i = 0;
  std::string header;
  if (!ExtractQuoted(line, i, header)) return;

  // Always observe players from any log line that has a quoted header (connect/disconnect/etc).
  // This lets `ru admins add <name_fragment>` resolve names even if the player hasn't typed `ru ...` yet.
  {
    std::string name;
    int slot = -1;
    uint64_t steamid64 = 0;
    ParseHeader(header, name, slot, steamid64);
    ObservePlayer(steamid64, name);
    ObserveSlotIdentity(slot, steamid64, name);

    // Best-effort connect/disconnect events for MAT.
    if (steamid64 != 0 && !isChat) {
      if (line.find(" connected, address") != std::string::npos) {
        WebhookEmitPlayerConnect(WebhookPlayer{steamid64, name, TeamForSteamid(steamid64)});
      } else if (line.find("\" disconnected") != std::string::npos) {
        WebhookEmitPlayerDisconnect(WebhookPlayer{steamid64, name, TeamForSteamid(steamid64)});
      }
    }
  }

  // Best-effort kill parsing to build per-player K/D for round_end payload.
  // Typical: "A<..><steam><..>" ... killed "B<..><steam><..>" ... with "weapon"
  if (!isChat && line.find(" killed \"") != std::string::npos) {
    if (GameEventsListenerInstalled()) {
      // Engine events drive stats when installed.
      return;
    }
    // We already extracted attacker header; now extract victim header.
    std::string victimHeader;
    if (ExtractQuoted(line, i, victimHeader)) {
      std::string aName, vName;
      int aSlot = -1, vSlot = -1;
      uint64_t aSid = 0, vSid = 0;
      ParseHeader(header, aName, aSlot, aSid);
      ParseHeader(victimHeader, vName, vSlot, vSid);

      if (aSid != 0) {
        auto& s = g_kd[aSid];
        if (!aName.empty()) s.name = aName;
        s.team = TeamForSteamid(aSid);
        s.kills += 1;
      }
      if (vSid != 0) {
        auto& s = g_kd[vSid];
        if (!vName.empty()) s.name = vName;
        s.team = TeamForSteamid(vSid);
        s.deaths += 1;
      }
    }
  }
  // Chat is routed by the in-process listener / Host_Say detour, never from here.
}

static void HandleLogLine(const std::string& line) {
  // The in-process logging listener sees every line (including these) first
  // and drives lifecycle itself; reading the file too would double-count
  // rounds and double-emit webhooks.
  if (InProcessLogListenerActive()) return;

  // Welcome screen trigger (deduped against the in-process logging listener).
  WelcomeObserveLogLine(line);
  std::lock_guard<std::recursive_mutex> lk(g_lifecycleMu);
  LifecycleImpl(line);
}

static void ThreadMain(std::filesystem::path logsDir) {
  Print("log observer: watching directory: %s\n", logsDir.string().c_str());

  std::filesystem::path curFile;
  std::ifstream cur;
  std::streampos curPos = 0;

  for (;;) {
    // Prefer the dedicated con_logfile output if it exists.
    {
      const std::string csgoDir = GetCsgoDirFromModuleDir();
      if (!csgoDir.empty()) {
        const auto preferred = PreferredConsoleLogPath(csgoDir);
        std::error_code ec;
        if (std::filesystem::exists(preferred, ec) && preferred != curFile) {
          cur.close();
          curFile = preferred;
          cur.open(curFile);
          if (cur.good()) {
            cur.seekg(0, std::ios::end);
            curPos = cur.tellg();
            if (DebugEnabled()) {
              Print("log observer: following (preferred): %s\n", curFile.string().c_str());
            }
          } else {
            curFile.clear();
          }
        }
      }
    }

    // Pick newest log file each tick (covers rotations/map changes).
    std::filesystem::path newest;
    if (std::filesystem::is_directory(logsDir) && logsDir.filename() == "logs") {
      newest = FindNewestLogFile(logsDir);
    } else {
      newest = FindNewestFileRecursive(logsDir);
    }
    if (!newest.empty() && newest != curFile) {
      cur.close();
      curFile = newest;
      cur.open(curFile);
      if (cur.good()) {
        // Start at end to avoid replaying old logs.
        cur.seekg(0, std::ios::end);
        curPos = cur.tellg();
        if (DebugEnabled()) {
          Print("log observer: following: %s\n", curFile.string().c_str());
        }
      } else {
        if (DebugEnabled()) {
          Print("log observer: failed to open: %s\n", curFile.string().c_str());
        }
        curFile.clear();
      }
    }

    if (cur.good()) {
      cur.clear();
      cur.seekg(curPos);
      std::string line;
      while (std::getline(cur, line)) {
        HandleLogLine(line);
      }
      curPos = cur.tellg();

      // If tellg() fails at EOF, recover by seeking end next time.
      if (curPos < 0) {
        cur.clear();
        cur.seekg(0, std::ios::end);
        curPos = cur.tellg();
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
}

}  // namespace

void ObserveLifecycleLogLine(const std::string& line) {
  if (line.empty()) return;
  std::lock_guard<std::recursive_mutex> lk(g_lifecycleMu);
  LifecycleImpl(line);
}

void StartLogReceiver() {
  bool expected = false;
  if (!g_started.compare_exchange_strong(expected, true)) return;

  const std::string csgoDir = GetCsgoDirFromModuleDir();
  if (csgoDir.empty()) {
    PrintLine("log observer: failed to derive csgo dir; chat routing disabled.");
    return;
  }

  // CS2 builds differ in where they write server log files:
  // - some use `csgo/logs/*.log`
  // - some use `csgo/rpt/YYYY_MM_DD/*`
  // We'll prefer `logs` if it exists; otherwise fall back to `rpt`.
  std::filesystem::path logsDir = std::filesystem::path(csgoDir) / "logs";
  std::error_code ec;
  if (!std::filesystem::exists(logsDir, ec) || !std::filesystem::is_directory(logsDir, ec)) {
    std::filesystem::path rptDir = std::filesystem::path(csgoDir) / "rpt";
    if (std::filesystem::exists(rptDir, ec) && std::filesystem::is_directory(rptDir, ec)) {
      logsDir = rptDir;
    } else if (DebugEnabled()) {
      Print("log observer: neither logs/ nor rpt/ found under: %s\n", csgoDir.c_str());
    }
  }

  std::thread(ThreadMain, logsDir).detach();
}

}  // namespace readyup

