#pragma once

#include <string>

namespace readyup {

// Best-effort CenterHtml banner, modeled after CounterStrikeSharp:
// fires `show_survival_respawn_status` with HTML in `loc_token`.
//
// Returns false if unavailable on this build.
bool PrintCenterHtmlToSlot(int slot, const std::string& html, int durationSeconds);

// Strictly per-client variant: builds the same event but delivers it ONLY to
// `slot` through that client's legacy game event listener (CounterStrikeSharp's
// FireEventToClient path; engine-surface.json "LegacyGameEventListener", with the
// returned proxy RTTI-checked before use).
// Never falls back to a broadcast. Must be called on the server (GameFrame) thread.
//
// Returns false if the event manager or the per-client listener is unavailable.
bool PrintCenterHtmlToClientOnly(int slot, const std::string& html, int durationSeconds);

}  // namespace readyup

