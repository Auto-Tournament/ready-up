#pragma once

// The clickable Panorama HUD (hud.so, readyup.hud.v1 `screen`; the Auto Tournament HUD addon's
// at_ui.xml) for the match flow. Design: canvas "Auto Tournament HUD kit", page Buttons (v2).
//
// ReadyHudTick (ready_hud.cpp) builds each player's base screen (warmup card, knife side pick,
// pauses) as a spec and hands them all to HudScreensRun once a tick. On top of the base screen
// come, in this order:
//   - a menu the player opened with .menu: the admin menu for admins (pause, replay a round,
//     change map, end match with a confirm), help for everyone else. 30 s, or until a click.
//     (.help belongs to essentials when it is loaded, so there it stays the chat list.)
//   - a vote that concerns the player: their team's surrender vote (.gg), the other team's
//     round replay request (.stop).
// Each button stands for a chat command and runs as if the player typed it (ru_api
// run_chat_command), so the buttons and the typed commands never drift apart. Mid-round no screen
// takes the cursor: hud.so draws the buttons as typed commands (spec chips=1).

#include <cstdint>
#include <functional>
#include <string>

namespace readyup {

struct HumanIdentity;

// True when the clickable HUD draws the match screens: readyup.cfg hud_screens (default on), and
// hud.so is loaded with `screen` and can draw on this CS2 build. Off: the center-HTML panels.
bool HudScreensOn();

// A live round is running (not freeze time, not paused): nothing may take the cursor.
bool HudMidRound();

// One tick: shows every connected human their screen (menu, vote, else baseFor(h); "" = none)
// and hides it for players who left. Game thread.
void HudScreensRun(const std::function<std::string(const HumanIdentity&)>& baseFor);

// `.menu` (and `.help` when essentials does not own it): opens "help" or "admin" (admins only)
// for this player. False when it can't be shown (HUD off, mid-round, practice or another
// plugin's mode): answer in chat instead.
bool HudScreensOpenMenu(int slot, uint64_t steamid64, const std::string& which);

// A spec value: no line breaks, no '|' (it separates button fields).
std::string HudSpecText(const std::string& s);

}  // namespace readyup
