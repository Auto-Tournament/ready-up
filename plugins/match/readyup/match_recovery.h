#pragma once

namespace readyup::match_recovery {

// Boot-time recovery: restore match context and (best-effort) round backup.
void TryRecoverAsync();

}  // namespace readyup::match_recovery

