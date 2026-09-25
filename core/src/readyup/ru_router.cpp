#include "readyup/ru_router.h"

#include "readyup/admin_check.h"
#include "readyup/chat.h"
#include "readyup/config.h"
#include "readyup/features.h"
#include "readyup/logging.h"
#include "readyup/plugin_loader.h"
#include "readyup/selftest.h"
#include "readyup/version.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace {

static std::string Trim(std::string s) {
  auto is_ws = [](unsigned char c) { return std::isspace(c) != 0; };
  while (!s.empty() && is_ws(static_cast<unsigned char>(s.back()))) s.pop_back();
  size_t i = 0;
  while (i < s.size() && is_ws(static_cast<unsigned char>(s[i]))) ++i;
  if (i) s.erase(0, i);
  return s;
}

static std::vector<std::string> SplitWS(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (size_t i = 0; i < s.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (std::isspace(c) != 0) {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
      continue;
    }
    cur.push_back(static_cast<char>(c));
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

static bool ShouldProcess(uint64_t steamid64, const std::string& playerName, const std::string& text) {
  // Multiple interception paths can observe the same chat message (e.g. Host_Say detour
  // + in-process log listener). Keep a tiny short-lived cache to suppress duplicates.
  using Clock = std::chrono::steady_clock;
  static std::mutex mu;
  static std::deque<std::pair<Clock::time_point, std::string>> recent;

  const auto now = Clock::now();
  const auto ttl = std::chrono::milliseconds(400);

  {
    std::lock_guard<std::mutex> lock(mu);
    while (!recent.empty() && (now - recent.front().first) > ttl) recent.pop_front();
  }

  std::string key;
  key.reserve(text.size() + 48);
  if (steamid64 != 0) {
    key = std::to_string(steamid64);
  } else {
    key = playerName;
  }
  key.push_back('|');
  key += text;

  {
    std::lock_guard<std::mutex> lock(mu);
    for (const auto& it : recent) {
      if (it.second == key) return false;
    }
    recent.emplace_back(now, std::move(key));
    while (recent.size() > 32) recent.pop_front();
  }
  return true;
}

}  // namespace

namespace readyup {

// Only `.ru` is the core's; player commands (.r, .pause, ...) belong to plugins (readyup-match).
bool IsCoreChatCommand(const std::string& firstToken) { return firstToken == ".ru"; }

void RouteChatCommand(uint64_t steamid64, const std::string& playerName, const std::string& text, int slot) {
  const std::string t = Trim(text);
  Debug("ru: RouteChatCommand steamid64=%llu name=\"%s\" text=\"%s\"\n",
        static_cast<unsigned long long>(steamid64),
        playerName.c_str(),
        t.c_str());
  if (t.empty()) {
    DebugLine("ru: ignored (empty after trim)");
    return;
  }
  // Player chat needs a chat source and a way to answer; the console path always works.
  if (steamid64 != 0 && !FeatureEnabled(Feature::ChatCommands)) return;
  if (!ShouldProcess(steamid64, playerName, t)) {
    DebugLine("ru: ignored (dedupe)");
    return;
  }

  auto parts = SplitWS(t);
  if (parts.empty()) {
    DebugLine("ru: ignored (no parts)");
    return;
  }
  const std::string& first = parts[0];

  // Commands owned by a loaded plugin (never `.ru`). The plugin callback runs on the next
  // GameFrame, with the sender's slot when the ClientCommand hook saw the line.
  if (!IsCoreChatCommand(first) && plugins::TryDispatchChat(steamid64, playerName, t, slot)) {
    Debug("ru: \"%s\" queued for its plugin\n", first.c_str());
    return;
  }
  if (first != ".ru") {
    Debug("ru: ignored (not .ru or a plugin command) first=\"%s\"\n", first.c_str());
    return;
  }

  // `.ru` alone
  if (parts.size() == 1) {
    DebugLine("ru: cmd=.ru (version)");
    SendToChat((std::string("Ready Up ") + BuildVersion()).c_str());
    return;
  }

  const std::string cmd = parts[1];
  Debug("ru: cmd=%s argc=%zu\n", cmd.c_str(), parts.size() > 2 ? parts.size() - 2 : 0u);

  // `.ru <sub>` a plugin registered (register_ru_subcommand); runs on the next GameFrame.
  if (!plugins::IsCoreRuSubcommand(cmd) && plugins::TryDispatchRu(/*console=*/false, steamid64, playerName, t, slot)) {
    Debug("ru: \".ru %s\" queued for its plugin\n", cmd.c_str());
    return;
  }

  auto requireAdmin = [&]() -> bool {
    // Allow server console; otherwise require admin.
    if (steamid64 == 0) return true;
    if (!readyup::IsReadyUpAdmin(steamid64)) {
      SendToChat("Ready Up: not authorized");
      return false;
    }
    return true;
  };

  if (cmd == "plugin" || cmd == "plugins") {
    if (!requireAdmin()) return;
    const std::vector<std::string> args(parts.begin() + 2, parts.end());
    plugins::HandlePluginCommand(args, /*replyToChat=*/steamid64 != 0);
    return;
  }

  if (cmd == "version") {
    SendToChat((std::string("Ready Up ") + BuildVersion()).c_str());
    return;
  }

  if (cmd == "help") {
    SendToChat("Ready Up: admins: .ru plugin list|reload <name> | .ru reload | .ru selftest | .ru version");
    std::string subs;
    for (const auto& s : plugins::PluginRuSubcommands()) subs += (subs.empty() ? "" : " ") + s.substr(0, s.find(' '));
    if (!subs.empty()) SendToChat(("Ready Up: plugins: .ru " + subs).c_str());
    SendToChat("Ready Up: players: .help (match commands, when the match plugin is loaded)");
    return;
  }

  if (cmd == "selftest") {
    if (!requireAdmin()) return;
    // Full report to the server console; the summary (+ failing items) to chat.
    const SelftestResult r = RunSelftest(/*printToConsole=*/true);
    if (steamid64 != 0) {
      constexpr size_t kMaxChatLines = 6;
      for (size_t i = 0; i < r.failures.size(); ++i) {
        if (i == kMaxChatLines) {
          SendToChat(("Ready Up selftest: ... " + std::to_string(r.failures.size() - i) + " more (see console)").c_str());
          break;
        }
        SendToChat(("Ready Up selftest FAIL: " + r.failures[i]).c_str());
      }
      SendToChat(("Ready Up " + r.summary).c_str());
    }
    return;
  }

  if (cmd == "reload") {
    // Allow server console; otherwise require admin.
    if (steamid64 != 0 && !readyup::IsReadyUpAdmin(steamid64)) {
      SendToChat("Reload: not authorized");
      return;
    }

    std::string err;
    if (!ReloadCfg(&err)) {
      SendToChat((std::string("Reload: failed: ") + (err.empty() ? "unknown" : err)).c_str());
      return;
    }
    SendToChat("Ready Up: cfg reloaded.");
    return;
  }

  // `.ru <cmd> ...` reaches a chat command a plugin owns (`.ru fleet status` -> `.fleet status`);
  // the plugin does its own admin check.
  if (steamid64 != 0 && !IsCoreChatCommand("." + cmd)) {
    std::string rest = "." + cmd;
    for (size_t i = 2; i < parts.size(); ++i) rest += " " + parts[i];
    if (plugins::TryDispatchChat(steamid64, playerName, rest, slot)) return;
  }

  // Unknown `ru` command; ignore to avoid chat spam.
  Debug("ru: unknown subcommand \"%s\" ignored\n", cmd.c_str());
}

}  // namespace readyup

