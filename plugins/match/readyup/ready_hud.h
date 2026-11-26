#pragma once

#include <cstdint>
#include <string>

namespace readyup {

// The ready-up screens on the clickable HUD (hud_screens.h), GameFrame thread: the ready card in
// scrim / match warmup (who is ready, per side, the viewer highlighted), the knife round and its
// side pick, and pauses or a missing team on a live map. Without the HUD (no hud.so, hud_screens=0)
// or for players who turned it off, ready-up runs in chat (ChatUnlessHud); the old centre-HTML
// panel is gone. Disabled by readyup.cfg `ready_hud=0`.
void ReadyHudTick();

// True while ready_hud is on and the clickable HUD draws its screens. Thread-safe.
bool ReadyHudShowing();

// True when the clickable HUD shows ready-up, so flow updates (ready, countdown, knife) go to it
// instead of chat (players who turned it off still get chat: ChatHudOff).
bool HudReplacesChat();


}  // namespace readyup
