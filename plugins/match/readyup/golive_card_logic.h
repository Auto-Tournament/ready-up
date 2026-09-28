#pragma once

// Go-live card (golive_card.h), the engine-free half: when to send, when the card is done and
// why. Times are milliseconds on any monotonic clock. ctest `match_live_cards`.
//
// CS2's center panel ignores the event's duration: a panel only stays steady when it is re-sent
// every frame (docs/HUD.md "Refresh rules"). The card therefore re-sends on the ready HUD's
// cadence (readyup.cfg hud_resend_ms, default 0 = every tick) for the full golive_card_seconds.

#include <cstdint>

namespace readyup {

enum class GoLiveCardStop {
  None,
  NotLive,     // the map is not live any more
  LivePanel,   // a pause / forfeit countdown needs the panel (the live panel takes over)
  FreezeEnd,   // the go-live round's freeze time ended: the round is being played
  TimeUp,      // golive_card_seconds ran out
};

const char* GoLiveCardStopName(GoLiveCardStop why);

struct GoLiveCardClock {
  int64_t startMs = 0;      // first send (welcome_round_delay_ms after the go-live round start)
  int64_t untilMs = 0;      // last send at or before this (start + golive_card_seconds)
  int64_t lastSendMs = -1;  // -1: nothing sent yet
  bool freezeEnded = false; // round_freeze_end of the go-live round was seen
};

// start + seconds: the card is up for all of golive_card_seconds (seconds <= 0: nothing).
int64_t GoLiveCardUntilMs(int64_t startMs, int seconds);

enum class GoLiveCardAction { Wait, Send, Stop };

// What the tick does at nowMs. Stop sets *why (never None). resendMs: readyup.cfg hud_resend_ms
// (0 = every tick, like the ready HUD).
GoLiveCardAction GoLiveCardDecide(const GoLiveCardClock& c, int64_t nowMs, bool live, bool livePanelBusy,
                                  int resendMs, GoLiveCardStop* why);

}  // namespace readyup
