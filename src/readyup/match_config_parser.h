#pragma once

#include "readyup/webhook.h"

#include <optional>
#include <string>

namespace readyup {

// Parses MAT match config JSON into WebhookMatchContext.
// Accepts either:
// - MatchResponse wrapper: { ..., config: { ... }, ... }
// - Raw MatchConfig object: { matchid, team1, team2, ... }
std::optional<WebhookMatchContext> ParseWebhookMatchContextFromJson(const std::string& json, std::string* errOut);

}  // namespace readyup

