#pragma once

namespace readyup {

// Best-effort broadcast to in-game chat (server "say").
// This is intentionally defensive: if the required engine symbol isn't available,
// this becomes a no-op.
void AnnounceToChat(const char* msg);

// Same mechanism as AnnounceToChat(), but NOT gated behind ReadyUp debug flags.
// Intended for actual user-facing features (e.g. `ru admins`).
void SendToChat(const char* msg);

// Sends a raw chat payload (no "[ReadyUp]" prefix added).
// Payload may include CS-style color codes (best-effort).
void SendRawToChat(const char* payload);

}  // namespace readyup

