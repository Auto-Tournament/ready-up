#pragma once

#include <cstdint>
#include <string>

namespace readyup {

// Per-player center-HTML HUD (PrintCenterHtmlToClientOnly, GameFrame thread).
// It IS the ready-up UI: shown continuously from joining until the match goes
// live, so the periodic "type .r" chat reminders are skipped while it works
// (ReadyHudShowing()).
//
// - scrim_warmup / match_warmup: brand header, "X/Y ready", every roster player
//   with ✔/✖ per side (CT blue, T orange), the viewer's own entry highlighted,
//   a `.r` / `.ur` hint and a "need players on both sides" note. Scrim countdown
//   shown in the title line.
// - match_knife: "knife round starting" before the fight, hidden during it,
//   then the side-pick panel (winners: .stay/.switch, others: waiting) with the
//   seconds left.
// - Hidden once going live (go-live restart pending / match_live) and in
//   idle/practice.
//
// Never drawn over the welcome screen (WelcomeActiveForSteam, ~5s after a
// player's first team join). Re-sent immediately when the HTML changes,
// otherwise once a second with a 2s duration: the panel stays up without its
// fade-in restarting. Disabled by readyup.cfg `ready_hud=0`.
void ReadyHudTick();

// True while the HUD is enabled and per-client center HTML is being delivered
// (the last send succeeded). Thread-safe.
bool ReadyHudShowing();

// True when the center panel is on and reaching players, so flow updates
// (ready, countdown, knife) go to the panel instead of chat.
bool HudReplacesChat();

// Brand header shared by the welcome card and the HUD: optional
// `<img src='hud_logo_url' height=imgHeight>` + `hud_brand` (readyup.cfg).
// fontClass: e.g. "fontSize-l" (empty = none).
std::string HudBrandHtml(int imgHeight, const char* fontClass);

// `.ru hud test <n>` (admin): shows test HTML variant n to this player only for
// ~10 seconds (over the HUD and the welcome card). Thread-safe; the send happens
// on the GameFrame thread. Returns a short description of the variant, or an
// empty string if n is unknown.
std::string ReadyHudRequestTest(uint64_t steamid64, int variant);

// `.ru hud anim [hz] [seconds]` (admin): redraws an ease-out progress bar with a frame counter
// `hz` times a second (1-64, default 64) for `seconds` (1-30, default 10) to this player only,
// to measure how fast the client really redraws the panel (record the screen, count the distinct
// frame numbers per second). Thread-safe. Returns the effective "<hz> Hz for <s> s" text.
std::string ReadyHudRequestAnim(uint64_t steamid64, int hz, int seconds);
// GameFrame thread, every frame (not rate-limited like ReadyHudTick): sends due animation frames.
void ReadyHudAnimTick();
// Thread-safe: an animation test is running for this player (the HUD leaves the panel alone).
bool ReadyHudAnimActive(uint64_t steamid64);

}  // namespace readyup
