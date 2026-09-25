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

// Console settings (ru_warmup_*, ru_cfg_exec_enable, ru_demo_*, ru_series_end_kick_delay_*):
// a console / RCON / cfg-file change is saved in state.json (only values that differ from the
// built-in default) and re-applied by Restore() after a server restart. `<setting> default`
// drops the saved value and applies the built-in default.
//
// Plugin load, before anything changes a setting: records the built-in defaults.
void CaptureDefaults();
// Returns false when `line` is not one of these settings (the caller runs it as usual).
// Otherwise runs it through `run` (or handles `<setting> default` itself), saves the result and
// stores run's return value in *consumed.
bool ConsoleSetting(const std::string& line, bool (*run)(const std::string& line), bool* consumed);

}  // namespace readyup::persisted_settings

