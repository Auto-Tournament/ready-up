#pragma once

#include <optional>
#include <string>

namespace readyup::persisted_settings {

// Persist/clear RU initialization settings in state.json (local_store.h).
void PersistWebhookUrl(std::optional<std::string> baseEventsUrl);
void PersistHeartbeatUrl(std::optional<std::string> heartbeatUrl);
void PersistMatchToken(std::optional<std::string> token);
void PersistAdminsUrl(std::optional<std::string> url);
void PersistAdminsRefreshSeconds(std::optional<int> seconds);

// Plugin load: re-apply the persisted settings (reads memory, never blocks).
void Restore();

}  // namespace readyup::persisted_settings

