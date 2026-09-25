#pragma once

#include <optional>
#include <string>

namespace readyup::persisted_settings {

// Persist/clear RU initialization settings to Postgres (best-effort).
void PersistWebhookUrl(std::optional<std::string> baseEventsUrl);
void PersistHeartbeatUrl(std::optional<std::string> heartbeatUrl);
void PersistMatchToken(std::optional<std::string> token);
void PersistAdminsUrl(std::optional<std::string> url);
void PersistAdminsRefreshSeconds(std::optional<int> seconds);

// Restore persisted settings (best-effort) without stalling the server thread.
void RestoreFromDbAsync();

}  // namespace readyup::persisted_settings

