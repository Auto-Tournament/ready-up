#pragma once

// Go-live card: when a map goes live (all ready -> live, the knife winners' .stay/.switch/.ct/.t,
// a scrim going live, `ru match start`), everyone gets a center card for golive_card_seconds
// (readyup.cfg, default 10, 0 = off): "LIVE · GO GO GO", the teams with their sides, and the
// commands that work now (.p/.pause/.tech, .up/.unpause, .tac, .admin). HTML: card_html.h.
//
// Timing: the go-live mp_restartgame and CS2's "Match started" announcement wipe the center
// panel, so the card starts welcome_round_delay_ms after the go-live round start (the same wait
// as the welcome card, welcome.h), then is re-sent every second with a 2 s duration at
// RU_HTML_PRIO_NOTICE. It stops early when the map stops being live or the live panel has
// something to show (a pause, a forfeit countdown: ready_hud.cpp takes over). With the card off
// chat is as before (the "LIVE!" line only); when center HTML is unavailable one chat line lists
// the commands instead.

namespace readyup {

// The map just went live (thread-safe; call it with any lock held). restartPending: the go-live
// restart was only queued (`ru match start`): the card waits for the round start that follows.
void GoLiveCardArm(const char* reason, bool restartPending);

// Thread-safe: a round (re)started (round_start). A card that has not started yet waits it out.
void GoLiveCardObserveRoundStart();

// GameFrame thread: sends / refreshes the card.
void GoLiveCardTick();

}  // namespace readyup
