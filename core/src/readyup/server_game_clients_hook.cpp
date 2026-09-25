#include "readyup/server_game_clients_hook.h"
#include "readyup/plugin_loader.h"

#include "readyup/admin_check.h"
#include "readyup/ccommand.h"
#include "readyup/chat.h"
#include "readyup/chat_colors.h"
#include "readyup/config.h"
#include "readyup/engine_surface.h"
#include "readyup/logging.h"
#include "readyup/ru_router.h"
#include "readyup/slot_registry.h"
#include "readyup/webhook.h"
#include "readyup/welcome.h"

#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace readyup {
namespace {

constexpr const char* kSlotName = "ISource2GameClients::ClientCommand";

// Minimal ABI declarations matching Source2 SDK usage (see Metamod Source2 provider).
// On CS2 this is a tiny wrapper around an int.
struct CPlayerSlot {
  int value;
};

using ClientCommandFn = void (*)(void* thisptr, CPlayerSlot slot, const void* args);
ClientCommandFn g_origClientCommand = nullptr;

std::mutex g_installMu;
std::atomic<bool> g_hook_ok{false};
std::atomic<bool> g_failed{false};
std::string g_failDetail;  // guarded by g_installMu

static bool PatchVtable(void** vtable, int index, void* replacement, void** outOld) {
  if (!vtable || index < 0) return false;
  void** slot = &vtable[index];
  const uintptr_t addr = reinterpret_cast<uintptr_t>(slot);
  const uintptr_t page = addr & ~(static_cast<uintptr_t>(getpagesize() - 1));
  if (mprotect(reinterpret_cast<void*>(page), static_cast<size_t>(getpagesize()), PROT_READ | PROT_WRITE) != 0) {
    return false;
  }
  if (outOld) *outOld = *slot;
  *slot = replacement;
  mprotect(reinterpret_cast<void*>(page), static_cast<size_t>(getpagesize()), PROT_READ);
  return true;
}

static bool IsAdminSteam(uint64_t steamid64) {
  // Allow per-match admins from match config (plus global admins).
  if (auto ctxOpt = WebhookGetMatchContext()) {
    const auto& ctx = *ctxOpt;
    if (ctx.admins.find(steamid64) != ctx.admins.end()) return true;
  }
  return readyup::IsReadyUpAdmin(steamid64);
}

static std::string StripChatColorBytes(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    // Skip common CS chat color control bytes (1..16).
    if (c >= 1 && c <= 16) continue;
    out.push_back(static_cast<char>(c));
  }
  return out;
}

static bool HasPrefixAlready(const std::string& name, const std::string& prefix) {
  const std::string want = StripChatColorBytes(prefix);
  if (want.empty()) return false;
  const std::string stripped = StripChatColorBytes(name);
  return stripped.rfind(want + " ", 0) == 0;
}

static std::string CaptainPrefixFor(uint64_t steamid64) {
  auto ctxOpt = WebhookGetMatchContext();
  if (!ctxOpt) return {};
  const auto& ctx = *ctxOpt;
  if (steamid64 != 0 && steamid64 == ctx.team1_captain_steamid64) return CaptainPrefixTeam1();
  if (steamid64 != 0 && steamid64 == ctx.team2_captain_steamid64) return CaptainPrefixTeam2();
  return {};
}

static void Hook_ClientCommand(void* thisptr, CPlayerSlot slot, const void* args) {
  if (!g_origClientCommand) return;

  // Arguments come from the verified CCommand layout; anything unexpected => pure pass-through.
  const auto argv = ReadCCommandArgs(args);
  if (!argv || argv->empty()) {
    static std::atomic<bool> s_warned{false};
    if (!argv && !s_warned.exchange(true)) {
      PrintLine("client-command: could not read CCommand arguments (layout sanity check failed); passing commands through.");
    }
    return g_origClientCommand(thisptr, slot, args);
  }
  const std::string& cmd = (*argv)[0];

  const int slotNum = slot.value;
  const auto ident = GetSlotIdentity(slotNum);
  Debug("client-command: slot=%d cmd=\"%s\" argc=%zu ident=%s steamid64=%llu\n", slotNum, cmd.c_str(), argv->size(),
        ident ? "yes" : "no", static_cast<unsigned long long>(ident ? ident->steamid64 : 0ull));

  if (cmd == "jointeam") {
    // Tentative welcome trigger for an explicit `jointeam 2|3` (0 = auto-assign is left to the
    // confirmed log/event sources, which know the final team).
    const int req = argv->size() >= 2 ? std::atoi((*argv)[1].c_str()) : -1;
    if (req == 2 || req == 3) {
      WelcomeObserveTeamJoin(slotNum, req, ident ? ident->steamid64 : 0, ident ? ident->name : std::string(),
                             WelcomeSource::ClientCommand);
    }
    return g_origClientCommand(thisptr, slot, args);
  }

  if (cmd != "say" && cmd != "say_team") return g_origClientCommand(thisptr, slot, args);
  if (!ident) return g_origClientCommand(thisptr, slot, args);

  const bool isTeamOnly = (cmd == "say_team");
  const std::string msg = CCommandArgString(*argv);

  // Handle `.ru ...` directly here so we have a reliable sender (SteamID64 + name + slot).
  // `.ru` visibility is configurable (consume_ru_chat).
  if (msg.rfind(".ru", 0) == 0) {
    Debug("client-command: saw .ru msg=\"%s\"\n", msg.c_str());
    RouteChatCommand(ident->steamid64, ident->name, msg, slotNum);
    if (ConsumeRuChat()) return;
  }
  // `.r` (ready toggle) and friends; RouteChatCommand dedupes a repeat of the line above.
  if (msg.rfind(".r", 0) == 0) {
    Debug("client-command: saw .r msg=\"%s\"\n", msg.c_str());
    RouteChatCommand(ident->steamid64, ident->name, msg, slotNum);
    if (ConsumeReadyChat()) return;
  }
  // Plugin chat commands: routed here with the sender's slot (the log listener's copy of the
  // line is deduped); RU_CMD_HIDE swallows the line before the engine prints it.
  {
    std::string first = msg.substr(0, msg.find_first_of(" \t"));
    for (char& c : first) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    uint32_t flags = 0;
    if (first.size() > 1 && first != ".ru" && plugins::ChatCommandOwned(first, &flags)) {
      Debug("client-command: plugin chat command \"%s\" (hide=%d)\n", first.c_str(), (flags & RU_CMD_HIDE) ? 1 : 0);
      RouteChatCommand(ident->steamid64, ident->name, msg, slotNum);
      if (flags & RU_CMD_HIDE) return;
    }
  }

  const uint64_t sid = ident->steamid64;
  std::string pluginPrefix;
  const bool hasPluginPrefix = plugins::PluginChatPrefixFor(sid, &pluginPrefix);  // set_chat_name_prefix
  const bool isAdmin = !hasPluginPrefix && IsAdminSteam(sid);
  const std::string capPrefix = hasPluginPrefix ? pluginPrefix : CaptainPrefixFor(sid);
  if (!isAdmin && capPrefix.empty()) return g_origClientCommand(thisptr, slot, args);

  // Admin/captain prefix: relay a prefixed line and consume the original to avoid duplicates.
  // (A "true" prefix via a temporary name swap needs IVEngineServer::ClientCommand, which is not
  // part of the verified engine surface.)
  const std::string prefix = isAdmin ? AdminPrefix() : capPrefix;
  if (HasPrefixAlready(ident->name, prefix) || msg.empty()) return g_origClientCommand(thisptr, slot, args);
  std::string line;
  line.reserve(prefix.size() + ident->name.size() + msg.size() + 32);
  line += prefix;
  line += " ";
  if (isTeamOnly) line += "[TEAM] ";
  line += ident->name;
  line += ": ";
  line += msg;
  SendRawToChat(line.c_str());
}

}  // namespace

void InstallServerGameClientsHook(void* serverGameClientsIface) {
  if (!serverGameClientsIface || g_hook_ok.load(std::memory_order_acquire)) return;
  std::lock_guard<std::mutex> lk(g_installMu);
  if (g_hook_ok.load(std::memory_order_acquire)) return;

  int index = -1;
  std::string why;
  if (!VerifyEngineVtableSlot(kSlotName, serverGameClientsIface, &index, &why)) {
    // An unverified slot is final and logged once; a different interface object (another
    // *GameClients* factory name) is simply skipped.
    const VtableVerdict v = EngineVtableVerdict(kSlotName);
    if (!v.ok && !g_failed.exchange(true)) {
      g_failDetail = why;
      Print("client-command: ClientCommand hook NOT installed (%s)\n", why.c_str());
    } else if (v.ok) {
      Debug("client-command: skipping interface %p: %s\n", serverGameClientsIface, why.c_str());
    }
    return;
  }

  void** vt = *reinterpret_cast<void***>(serverGameClientsIface);
  void* old = nullptr;
  if (!PatchVtable(vt, index, reinterpret_cast<void*>(&Hook_ClientCommand), &old) || !old) {
    g_failDetail = "mprotect/patch of the vtable slot failed";
    g_failed.store(true);
    PrintLine("client-command: failed to patch vtable for ClientCommand.");
    return;
  }

  g_origClientCommand = reinterpret_cast<ClientCommandFn>(old);
  g_hook_ok.store(true, std::memory_order_release);
  MarkEngineVtablePatched(kSlotName, old);
  Print("client-command: hooked ISource2GameClients::ClientCommand vtbl[%d] (verified)\n", index);
}

bool ServerGameClientsHookInstalled() {
  return g_hook_ok.load(std::memory_order_acquire);
}

bool ServerGameClientsHookFailed(std::string* detail) {
  if (!g_failed.load(std::memory_order_acquire)) return false;
  if (detail) {
    std::lock_guard<std::mutex> lk(g_installMu);
    *detail = g_failDetail;
  }
  return true;
}

bool ForceJoinTeamForSlot(int /*slot*/, int /*joinTeam*/) {
  return false;
}

}  // namespace readyup
