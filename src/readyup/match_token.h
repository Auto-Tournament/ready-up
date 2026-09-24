#pragma once

#include <mutex>
#include <optional>
#include <string>

namespace readyup {

// Match config auth token (set via ru_match_token).
// Used for match config fetch, webhooks, heartbeat, and MAT-admins fetch.
std::optional<std::string> GetMatchTokenCopy();

// Set match token (empty string clears it). Also configures:
// - Webhook bearer token
// - MAT-admins bearer token
void SetMatchToken(std::string tokenOrEmpty);

}  // namespace readyup

