#include "readyup/persisted_settings.h"

#include "readyup/config.h"
#include "readyup/logging.h"
#include "readyup/mat_admins.h"
#include "readyup/match_token.h"
#include "readyup/db_writer.h"
#include "readyup/postgres.h"
#include "readyup/workers.h"
#include "readyup/webhook.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace readyup::persisted_settings {
namespace {

static constexpr const char* kKeyWebhookUrl = "ru_webhook_url";
static constexpr const char* kKeyHeartbeatUrl = "ru_heartbeat_url";
static constexpr const char* kKeyMatchToken = "ru_match_token";
static constexpr const char* kKeyAdminsUrl = "ru_admins_url";
static constexpr const char* kKeyAdminsRefreshSeconds = "ru_admins_refresh_seconds";

// Ordered, on the plugin's DB writer thread (db_writer.h).
static void PersistAsync(std::string key, std::optional<std::string> value) {
  db_writer::SetSettingAsync(std::move(key), std::move(value));
}

static int ClampRefreshSeconds(int seconds) {
  return std::max(10, std::min(3600, seconds));
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

void RestoreFromDbAsync() {
  static std::once_flag once;
  std::call_once(once, [] {
    if (!pg::Available()) return;

    workers::Spawn("settings-restore", [] {
      std::string err;
      if (!pg::EnsureSchema(&err)) {
        if (readyup::DebugEnabled()) readyup::Debug("persisted_settings: EnsureSchema failed: %s\n", err.c_str());
        return;
      }

      auto get = [&](const char* k) -> std::optional<std::string> {
        std::string e;
        auto v = pg::GetSetting(k, &e);
        if (!e.empty() && readyup::DebugEnabled()) readyup::Debug("persisted_settings: GetSetting(%s) err=%s\n", k, e.c_str());
        if (v && v->empty()) return std::nullopt;
        return v;
      };

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
          const int s = std::stoi(*adminsRefresh);
          readyup::mat_admins::ConfigureRefreshSeconds(s);
        } catch (...) {
        }
      }

      if (readyup::DebugEnabled()) {
        readyup::Debug("persisted_settings: restored webhook=%s heartbeat=%s token=%s admins_url=%s admins_refresh=%s\n",
                       webhookUrl ? "1" : "0",
                       heartbeatUrl ? "1" : "0",
                       matchToken ? "1" : "0",
                       adminsUrl ? "1" : "0",
                       adminsRefresh ? "1" : "0");
      }
    });
  });
}

}  // namespace readyup::persisted_settings

