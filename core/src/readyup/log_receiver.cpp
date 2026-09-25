#include "readyup/log_receiver.h"

#include "readyup/plugin_api.h"
#include "readyup/plugin_loader.h"

#include "readyup/client_command_hook.h"
#include "readyup/config.h"
#include "readyup/game_events.h"
#include "readyup/logging.h"
#include "readyup/path.h"
#include "readyup/slot_registry.h"
#include "readyup/steamid.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace readyup {
namespace {

std::atomic<bool> g_started{false};

// Round counter for log-derived RU_EVENT_ROUND_START/END (reset by Match_Start / map change).
int g_roundNumber = 0;
int g_ctScore = 0;
int g_tScore = 0;

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

static std::recursive_mutex g_lifecycleMu;

// Normalized plugin lifecycle events derived from server log lines. Shared by the in-process
// logging listener (client_command_hook.cpp, the primary source) and the file reader below
// (fallback only when the in-process listener is not installed). Round events only while
// engine game events are not delivered (they drive the lifecycle then). The match policy
// (knife round, webhooks, scores) lives in plugins/match, which sees every line through
// subscribe_log_line.
static void LifecycleImpl(const std::string& line) {
  // Chat lines can contain arbitrary text (e.g. a player typing
  // `World triggered "Round_Start"`); never treat them as world/map events.
  const bool isChat =
      line.find("\" say \"") != std::string::npos || line.find("\" say_team \"") != std::string::npos;
  if (isChat) return;

  if (line.find("Loading map \"") != std::string::npos || line.find("Started map \"") != std::string::npos) {
    std::string map;
    if (ExtractBetween(line, "map \"", "\"", map) && !map.empty()) {
      plugins::LifecycleEvent e;
      e.type = RU_EVENT_MAP_START;
      e.source = RU_SOURCE_LOG;
      e.map = map;
      plugins::PostEvent(std::move(e));  // deduped (Loading + Started map)
      g_roundNumber = 0;
      g_ctScore = g_tScore = 0;
    }
    return;
  }

  // mp_restartgame / warmup end: CS2 logs `World triggered "Match_Start"` right before the
  // first Round_Start of the fresh game.
  if (line.find("World triggered \"Match_Start\"") != std::string::npos) {
    plugins::LifecycleEvent e;
    e.type = RU_EVENT_MATCH_START;
    e.source = RU_SOURCE_LOG;
    plugins::PostEvent(std::move(e));
    g_roundNumber = 0;
    g_ctScore = g_tScore = 0;
    return;
  }

  if (line.find("World triggered \"Round_Start\"") != std::string::npos) {
    if (GameEventsListenerInstalled()) return;  // engine events post the round events
    g_roundNumber += 1;
    plugins::LifecycleEvent e;
    e.type = RU_EVENT_ROUND_START;
    e.source = RU_SOURCE_LOG;
    e.round = g_roundNumber;
    e.team_ct_score = g_ctScore;
    e.team_t_score = g_tScore;
    plugins::PostEvent(std::move(e));
    return;
  }

  // End-of-round notices carry CT/T scores, e.g.
  //   Team "TERRORIST" triggered "SFUI_Notice_Terrorists_Win" (CT "0") (T "1")
  if (line.find("triggered \"SFUI_Notice_") != std::string::npos) {
    int ct = 0, tt = 0;
    if (ParseScorePair(line, "(CT \"", "(T \"", ct, tt)) {
      g_ctScore = ct;
      g_tScore = tt;
    }
    if (GameEventsListenerInstalled()) return;
    if (g_roundNumber == 0) g_roundNumber = 1;
    plugins::LifecycleEvent e;
    e.type = RU_EVENT_ROUND_END;
    e.source = RU_SOURCE_LOG;
    e.round = g_roundNumber;
    e.winner = line.find("CTs_Win") != std::string::npos ? 3 : line.find("Terrorists_Win") != std::string::npos ? 2 : 0;
    e.team_ct_score = g_ctScore;
    e.team_t_score = g_tScore;
    plugins::PostEvent(std::move(e));
    return;
  }
}

static void HandleLogLine(const std::string& line) {
  // The in-process logging listener sees every line (including these) first; reading the file
  // too would post every event twice.
  if (InProcessLogListenerActive()) return;
  if (line.find(kLogPrefix) != std::string::npos) return;  // our own output
  plugins::PostLogLine(line);
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
  plugins::PostLogLine(line);  // subscribe_log_line (every source funnels through here)
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

