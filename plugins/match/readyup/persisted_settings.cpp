#include "readyup/persisted_settings.h"

#include "readyup/config.h"
#include "readyup/demo_recorder.h"
#include "readyup/logging.h"
#include "readyup/mat_admins.h"
#include "readyup/match_end.h"
#include "readyup/match_token.h"
#include "readyup/local_store.h"
#include "readyup/modes.h"
#include "readyup/webhook.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace readyup::persisted_settings {
namespace {

static constexpr const char* kKeyWebhookUrl = "ru_webhook_url";
static constexpr const char* kKeyHeartbeatUrl = "ru_heartbeat_url";
static constexpr const char* kKeyMatchToken = "ru_match_token";
static constexpr const char* kKeyAdminsUrl = "ru_admins_url";
static constexpr const char* kKeyAdminsRefreshSeconds = "ru_admins_refresh_seconds";

// state.json (local_store.h): memory at once, saved by the store's writer thread.
static void PersistAsync(std::string key, std::optional<std::string> value) {
  local_store::SetSetting(key, std::move(value));
}

static int ClampRefreshSeconds(int seconds) {
  return std::max(10, std::min(3600, seconds));
}

// ---- console settings ----------------------------------------------------------------------
// Each setting is saved under its command name as the value its getter reads back. Only values
// that differ from the built-in default (captured at plugin load) are kept. Restore order
// matters: startmoney before maxmoney (setting startmoney raises maxmoney).
struct ConsoleEntry {
  const char* key;
  std::string (*get)();
  void (*set)(const std::string&);
};

std::string B(bool v) { return v ? "1" : "0"; }
bool PB(const std::string& v) { return v == "1"; }
int PI(const std::string& v) { return std::atoi(v.c_str()); }
int Kick(int which) {
  int k[3] = {0, 0, 0};
  GetSeriesEndKickDelays(&k[0], &k[1], &k[2]);
  return k[which];
}
// Upload headers as "Name: value" lines (names never hold ':' or newlines, values no newlines).
std::string HeadersGet() {
  std::string out;
  for (const auto& h : demo::Get().uploadHeaders) out += h.first + ": " + h.second + "\n";
  return out;
}
void HeadersSet(const std::string& v) {
  demo::ClearUploadHeaders();
  size_t start = 0;
  while (start < v.size()) {
    size_t end = v.find('\n', start);
    if (end == std::string::npos) end = v.size();
    const std::string line = v.substr(start, end - start);
    const size_t c = line.find(": ");
    if (c != std::string::npos) demo::SetUploadHeader(line.substr(0, c), line.substr(c + 2));
    start = end + 1;
  }
}

const std::vector<ConsoleEntry>& Entries() {
  static const std::vector<ConsoleEntry> k = {
      {"ru_cfg_exec_enable", [] { return B(CfgExecEnabled()); }, [](const std::string& v) { SetCfgExecEnabled(PB(v)); }},
      {"ru_warmup_enable", [] { return B(WarmupEnabled()); }, [](const std::string& v) { SetWarmupEnabled(PB(v)); }},
      {"ru_warmup_message_html", [] { return WarmupHtmlCustom(); },
       [](const std::string& v) { SetWarmupHtmlMessage(v); }},
      {"ru_warmup_respawn", [] { return B(WarmupRespawnEnabled()); },
       [](const std::string& v) { SetWarmupRespawnEnabled(PB(v)); }},
      {"ru_warmup_ignore_win_conditions", [] { return B(WarmupIgnoreWinConditions()); },
       [](const std::string& v) { SetWarmupIgnoreWinConditions(PB(v)); }},
      {"ru_warmup_roundtime_minutes", [] { return std::to_string(WarmupRoundTimeMinutes()); },
       [](const std::string& v) { SetWarmupRoundTimeMinutes(PI(v)); }},
      {"ru_warmup_startmoney", [] { return std::to_string(WarmupStartMoney()); },
       [](const std::string& v) { SetWarmupStartMoney(PI(v)); }},
      {"ru_warmup_maxmoney", [] { return std::to_string(WarmupMaxMoney()); },
       [](const std::string& v) { SetWarmupMaxMoney(PI(v)); }},
      {"ru_warmup_buy_anywhere", [] { return B(WarmupBuyAnywhereEnabled()); },
       [](const std::string& v) { SetWarmupBuyAnywhereEnabled(PB(v)); }},
      {"ru_warmup_infinite_ammo", [] { return B(WarmupInfiniteAmmoEnabled()); },
       [](const std::string& v) { SetWarmupInfiniteAmmoEnabled(PB(v)); }},
      {"ru_demo_recording_enabled", [] { return B(demo::Get().recordingEnabled); },
       [](const std::string& v) { demo::SetRecordingEnabled(PB(v)); }},
      {"ru_demo_path", [] { return demo::Get().path; }, [](const std::string& v) { (void)demo::SetPath(v, nullptr); }},
      {"ru_demo_name_format", [] { return demo::Get().nameFormat; },
       [](const std::string& v) { demo::SetNameFormat(v); }},
      {"ru_demo_upload_url", [] { return demo::Get().uploadUrl; }, [](const std::string& v) { demo::SetUploadUrl(v); }},
      {"ru_demo_upload_method", [] { return demo::Get().uploadMethod; },
       [](const std::string& v) { (void)demo::SetUploadMethod(v); }},
      {"ru_demo_upload_headers", &HeadersGet, &HeadersSet},
      {"ru_demo_upload_attempts", [] { return std::to_string(demo::Get().uploadAttempts); },
       [](const std::string& v) { demo::SetUploadAttempts(PI(v)); }},
      {"ru_series_end_kick_delay_no_demo", [] { return std::to_string(Kick(0)); },
       [](const std::string& v) { SetSeriesEndKickDelays(std::max(0, PI(v)), -1, -1); }},
      {"ru_series_end_kick_delay_demo_no_upload", [] { return std::to_string(Kick(1)); },
       [](const std::string& v) { SetSeriesEndKickDelays(-1, std::max(0, PI(v)), -1); }},
      {"ru_series_end_kick_delay_demo_upload", [] { return std::to_string(Kick(2)); },
       [](const std::string& v) { SetSeriesEndKickDelays(-1, -1, std::max(0, PI(v))); }},
  };
  return k;
}

std::mutex g_defaultsMu;
std::vector<std::string> g_defaults;  // Entries() order; empty until CaptureDefaults()

// The store reads "" as "not set": an empty value (e.g. ru_demo_path "") is saved as "\"\"".
constexpr const char* kEmpty = "\"\"";

// The console command -> its entry. The headers are set by two commands and saved as one list.
const ConsoleEntry* EntryForCommand(const std::string& cmd, size_t* index) {
  std::string key = cmd;
  if (cmd == "ru_demo_upload_header" || cmd == "ru_demo_upload_headers_clear") key = "ru_demo_upload_headers";
  else if (cmd == "ru_demo_upload_headers") return nullptr;  // not a command
  const auto& es = Entries();
  for (size_t i = 0; i < es.size(); ++i) {
    if (key == es[i].key) {
      *index = i;
      return &es[i];
    }
  }
  return nullptr;
}

// Saves the setting's live value when it differs from the default, else clears the saved one.
// ru_warmup_startmoney can raise maxmoney, so that one saves maxmoney too.
void SyncConsoleSetting(size_t idx) {
  std::lock_guard<std::mutex> lk(g_defaultsMu);
  const auto& es = Entries();
  if (g_defaults.size() != es.size() || idx >= es.size()) return;
  const size_t last = std::strcmp(es[idx].key, "ru_warmup_startmoney") == 0 ? idx + 1 : idx;
  for (size_t i = idx; i <= last && i < es.size(); ++i) {
    const std::string v = es[i].get();
    if (v == g_defaults[i]) local_store::SetSetting(es[i].key, std::nullopt);
    else local_store::SetSetting(es[i].key, v.empty() ? std::string(kEmpty) : v);
  }
}

// Console tokens; "quoted strings" stay together (as match_end.cpp SplitArgs).
std::vector<std::string> Tokens(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  bool inQ = false, any = false;
  for (char c : line) {
    if (c == '"') {
      inQ = !inQ;
      any = true;
      continue;
    }
    if (!inQ && (c == ' ' || c == '\t' || c == '\r' || c == '\n')) {
      if (any || !cur.empty()) out.push_back(cur);
      cur.clear();
      any = false;
      continue;
    }
    cur.push_back(c);
  }
  if (any || !cur.empty()) out.push_back(cur);
  return out;
}

}  // namespace

void PersistWebhookUrl(std::optional<std::string> baseEventsUrl) {
  if (baseEventsUrl && baseEventsUrl->empty()) baseEventsUrl.reset();
  PersistAsync(kKeyWebhookUrl, std::move(baseEventsUrl));
}

void PersistHeartbeatUrl(std::optional<std::string> heartbeatUrl) {
  if (heartbeatUrl && heartbeatUrl->empty()) heartbeatUrl.reset();
  PersistAsync(kKeyHeartbeatUrl, std::move(heartbeatUrl));
}

void PersistMatchToken(std::optional<std::string> token) {
  if (token && token->empty()) token.reset();
  PersistAsync(kKeyMatchToken, std::move(token));
}

void PersistAdminsUrl(std::optional<std::string> url) {
  if (url && url->empty()) url.reset();
  PersistAsync(kKeyAdminsUrl, std::move(url));
}

void PersistAdminsRefreshSeconds(std::optional<int> seconds) {
  if (!seconds.has_value()) {
    PersistAsync(kKeyAdminsRefreshSeconds, std::nullopt);
    return;
  }
  PersistAsync(kKeyAdminsRefreshSeconds, std::to_string(ClampRefreshSeconds(*seconds)));
}

void Restore() {
  auto get = [](const char* k) { return local_store::GetSetting(k); };
  const auto webhookUrl = get(kKeyWebhookUrl);
  const auto heartbeatUrl = get(kKeyHeartbeatUrl);
  const auto matchToken = get(kKeyMatchToken);
  const auto adminsUrl = get(kKeyAdminsUrl);
  const auto adminsRefresh = get(kKeyAdminsRefreshSeconds);

  if (webhookUrl) {
    WebhookConfigure(*webhookUrl);
    WebhookStartSenderThread();
  }
  if (heartbeatUrl) {
    WebhookConfigureHeartbeatUrl(*heartbeatUrl);
    WebhookStartSenderThread();
  }
  if (matchToken) {
    readyup::SetMatchToken(*matchToken);
  }
  if (adminsUrl) {
    readyup::mat_admins::ConfigureAdminsUrl(*adminsUrl);
  }
  if (adminsRefresh) {
    try {
      readyup::mat_admins::ConfigureRefreshSeconds(std::stoi(*adminsRefresh));
    } catch (...) {
    }
  }

  int console = 0;
  for (const auto& e : Entries()) {
    if (auto v = get(e.key)) {
      e.set(*v == kEmpty ? std::string() : *v);
      ++console;
    }
  }
  if (console > 0) Print("match: restored %d console setting(s) from state.json\n", console);

  if (readyup::DebugEnabled()) {
    readyup::Debug("persisted_settings: restored webhook=%s heartbeat=%s token=%s admins_url=%s admins_refresh=%s\n",
                   webhookUrl ? "1" : "0", heartbeatUrl ? "1" : "0", matchToken ? "1" : "0", adminsUrl ? "1" : "0",
                   adminsRefresh ? "1" : "0");
  }
}

void CaptureDefaults() {
  std::lock_guard<std::mutex> lk(g_defaultsMu);
  g_defaults.clear();
  for (const auto& e : Entries()) g_defaults.push_back(e.get());
}

bool ConsoleSetting(const std::string& line, bool (*run)(const std::string& line), bool* consumed) {
  const auto args = Tokens(line);
  if (args.empty()) return false;
  size_t idx = 0;
  const ConsoleEntry* e = EntryForCommand(args[0], &idx);
  if (!e) return false;
  if (args.size() == 2 && args[1] == "default") {
    std::string def;
    {
      std::lock_guard<std::mutex> lk(g_defaultsMu);
      if (idx < g_defaults.size()) def = g_defaults[idx];
    }
    e->set(def);
    const bool multiLine = def.find('\n') != std::string::npos;
    Print("%s: back to the default (%s); saved value cleared\n", args[0].c_str(),
          def.empty() ? "empty" : (multiLine ? "headers" : def.c_str()));
    *consumed = true;
  } else {
    *consumed = run ? run(line) : false;
  }
  SyncConsoleSetting(idx);
  return true;
}

}  // namespace readyup::persisted_settings

