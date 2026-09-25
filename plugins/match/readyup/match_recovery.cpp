#include "readyup/match_recovery.h"

#include "readyup/engine.h"
#include "readyup/config.h"
#include "readyup/logging.h"
#include "readyup/match_config_parser.h"
#include "readyup/match_state.h"
#include "readyup/modes.h"
#include "readyup/persisted_match_state.h"
#include "readyup/webhook.h"
#include "readyup/workers.h"

#include <chrono>
#include <optional>
#include <string>
#include <thread>

namespace readyup::match_recovery {
namespace {

static void RecoverWorker() {
  auto jsonOpt = readyup::persisted_match_state::GetActiveMatchJson();
  if (!jsonOpt) return;

  std::string parseErr;
  auto ctxOpt = readyup::ParseWebhookMatchContextFromJson(*jsonOpt, &parseErr);
  if (!ctxOpt) {
    if (DebugEnabled()) {
      Debug("match_recovery: parse failed: %s\n", parseErr.empty() ? "(unknown)" : parseErr.c_str());
    }
    return;
  }

  WebhookStartSenderThread();
  WebhookSetMatchContext(*ctxOpt);
  // Boot mode is idle; a restored match needs match_warmup gating.
  SetModeMatchWarmupForRecovery();

  // Ensure we're in Ready Up warmup logic and waiting for players.
  ClearReadyStates();
  WebhookSetHeartbeatStatus("warmup");

  // Apply match cvars immediately (no restart).
  ApplyMatchCvarsNow();

  // If this match was live, attempt an in-server backup restore and gate resume until ready.
  const bool wasLive = readyup::persisted_match_state::GetLiveFlag();
  if (wasLive) {
    SetRecoveryGate(true);

    // Best-effort: pause immediately, restore round backup, then pause again.
    (void)EnqueueServerCommand("mp_backup_restore_load_autopause 1");
    (void)EnqueueServerCommand("mp_pause_match");

    if (auto fileOpt = readyup::persisted_match_state::GetBackupFile()) {
      const std::string cmd = "mp_backup_restore_load_file " + *fileOpt;
      (void)EnqueueServerCommand(cmd.c_str());
      if (DebugEnabled()) {
        Debug("match_recovery: restore: %s\n", cmd.c_str());
      }
    } else if (DebugEnabled()) {
      Debug("match_recovery: no backup file persisted; skipping restore\n");
    }

    (void)EnqueueServerCommand("mp_pause_match");

    // Let MAT know we are attempting recovery (UI-side rebuild).
    const auto ms = MatchStateGet();
    const int mapNumber = ms.map_number <= 0 ? 1 : ms.map_number;
    const int roundNumber = readyup::persisted_match_state::GetLastRoundNumber().value_or(ms.round_number);
    WebhookEmitRecoverRequested(mapNumber, roundNumber);
  }

  if (DebugEnabled()) {
    Debug("match_recovery: restored matchid=%llu live=%s\n",
          static_cast<unsigned long long>(ctxOpt->matchid),
          wasLive ? "1" : "0");
  }
}

}  // namespace

void TryRecoverAsync() {
  workers::Spawn("match-recovery", [] {
    // Give other init a moment to run first (RCON init, schema ensure, etc).
    if (!workers::SleepFor(std::chrono::milliseconds(500))) return;
    RecoverWorker();
  });
}

}  // namespace readyup::match_recovery

