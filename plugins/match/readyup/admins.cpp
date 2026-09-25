#include "readyup/admins.h"

#include "readyup/engine.h"
#include "readyup/logging.h"
#include "readyup/admin_check.h"
#include "readyup/player_registry.h"
#include "readyup/local_store.h"

#include <algorithm>
#include <cctype>

namespace readyup {
namespace {

static bool IsDigits(const std::string& s) {
  return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos;
}

static uint64_t ParseU64(const std::string& s) {
  return static_cast<uint64_t>(std::strtoull(s.c_str(), nullptr, 10));
}

static std::string Join(const std::vector<std::string>& v, size_t start) {
  std::string out;
  for (size_t i = start; i < v.size(); ++i) {
    if (!out.empty()) out.push_back(' ');
    out.append(v[i]);
  }
  return out;
}

static bool SenderAuthorized(uint64_t senderSteamid64, std::string* err) {
  // Server console is always authorized.
  if (senderSteamid64 == 0) return true;

  (void)err;
  return IsReadyUpAdmin(senderSteamid64);
}

static void Reply(uint64_t senderSteamid64, const std::string& msg) {
  // If the server console invoked the command, don't broadcast to in-game chat.
  if (senderSteamid64 != 0) {
    SendToChat(msg.c_str());
  }
  Print("%s\n", msg.c_str());
}

}  // namespace

void HandleAdminsCommand(uint64_t senderSteamid64, const std::string& senderName, const std::vector<std::string>& args) {
  (void)senderName;
  Debug("admins: sender steamid64=%llu name=\"%s\" argc=%zu\n",
        static_cast<unsigned long long>(senderSteamid64),
        senderName.c_str(),
        args.size());

  const bool fleet = local_store::FleetMode();
  if (args.empty() || args[0] == "list") {
    DebugLine("admins: list");
    int64_t rev = -1;
    const auto admins = fleet ? local_store::FleetAdmins(&rev) : local_store::LocalAdmins();
    const std::string source =
        fleet ? (rev < 0 ? std::string("platform, none received yet") : "platform, rev " + std::to_string(rev))
              : std::string("admins.json");
    if (admins.empty()) {
      Reply(senderSteamid64, "Admins (" + source + "): (none)");
      return;
    }

    // Keep chat readable.
    const size_t maxLines = 8;
    Reply(senderSteamid64, "Admins (" + source + "):");
    for (size_t i = 0; i < admins.size() && i < maxLines; ++i) {
      const auto& a = admins[i];
      Reply(senderSteamid64, " - " + (a.name.empty() ? std::string("?") : a.name) + " (" + std::to_string(a.steamid64) + ")");
    }
    if (admins.size() > maxLines) {
      Reply(senderSteamid64, "...and " + std::to_string(admins.size() - maxLines) + " more");
    }
    return;
  }

  const std::string sub = args[0];
  if (sub != "add" && sub != "remove") {
    Debug("admins: bad subcmd \"%s\"\n", sub.c_str());
    Reply(senderSteamid64, "Usage: .ru admins [list|add|remove] <steamid64|name_fragment>");
    return;
  }
  if (fleet) {
    Reply(senderSteamid64, "Admins are managed on the platform (fleet mode); `ru admins list` shows them.");
    return;
  }
  if (args.size() < 2) {
    Reply(senderSteamid64, "Usage: .ru admins " + sub + " <steamid64|name_fragment>");
    return;
  }

  // Auth:
  // - Server console (steamid64=0) is always authorized and is the ONLY way to create the first admin.
  // - Players must already be admins to add/remove admins.
  std::string err;
  const bool empty = local_store::LocalAdmins().empty();

  if (senderSteamid64 != 0) {
    if (empty) {
      Reply(senderSteamid64,
            "Admins: no admins configured yet. The first admin must be added from the server console (or in admins.json).");
      return;
    }
    err.clear();
    if (!SenderAuthorized(senderSteamid64, &err)) {
      Debug("admins: SenderAuthorized=false err=\"%s\"\n", err.c_str());
      Reply(senderSteamid64, err.empty() ? "Admins: not authorized" : ("Admins: auth error: " + err));
      return;
    }
  }

  const std::string target = Join(args, 1);
  Debug("admins: sub=%s target=\"%s\"\n", sub.c_str(), target.c_str());
  uint64_t targetSteamid64 = 0;
  std::string targetName;

  if (IsDigits(target)) {
    DebugLine("admins: target is digits -> steamid64 direct");
    targetSteamid64 = ParseU64(target);
    // If we observed this player, reuse their current name.
    auto players = ListObservedPlayers();
    for (const auto& p : players) {
      if (p.steamid64 == targetSteamid64) {
        targetName = p.name;
        break;
      }
    }
    if (targetName.empty()) targetName = "Admin";
  } else {
    DebugLine("admins: target is name fragment -> ClosestPlayerMatch");
    std::string matchErr;
    auto m = ClosestPlayerMatch(target, &matchErr);
    if (!m) {
      Debug("admins: ClosestPlayerMatch failed err=\"%s\"\n", matchErr.c_str());
      Reply(senderSteamid64, "Admins: " + matchErr);
      return;
    }
    targetSteamid64 = m->steamid64;
    targetName = m->name;
  }

  if (targetSteamid64 == 0) {
    Reply(senderSteamid64, "Admins: failed to resolve target SteamID64.");
    return;
  }

  if (sub == "add") {
    Debug("admins: add steamid64=%llu name=\"%s\"\n",
          static_cast<unsigned long long>(targetSteamid64),
          targetName.c_str());
    if (!local_store::AddLocalAdmin(targetSteamid64, targetName)) {
      Reply(senderSteamid64, "Admins: already admin (" + std::to_string(targetSteamid64) + ")");
      return;
    }
    Reply(senderSteamid64, "Admins: added " + targetName + " (" + std::to_string(targetSteamid64) + ")");
    return;
  }

  // remove
  Debug("admins: remove steamid64=%llu\n", static_cast<unsigned long long>(targetSteamid64));
  if (!local_store::RemoveLocalAdmin(targetSteamid64)) {
    Reply(senderSteamid64, "Admins: not an admin (" + std::to_string(targetSteamid64) + ")");
    return;
  }
  Reply(senderSteamid64, "Admins: removed (" + std::to_string(targetSteamid64) + ")");
}

}  // namespace readyup
