#include "readyup/golive_card_logic.h"

namespace readyup {

const char* GoLiveCardStopName(GoLiveCardStop why) {
  switch (why) {
    case GoLiveCardStop::None: return "none";
    case GoLiveCardStop::NotLive: return "not live any more";
    case GoLiveCardStop::LivePanel: return "live panel took over (pause / forfeit)";
    case GoLiveCardStop::FreezeEnd: return "freeze time ended";
    case GoLiveCardStop::TimeUp: return "time up";
  }
  return "?";
}

int64_t GoLiveCardUntilMs(int64_t startMs, int seconds) {
  return startMs + (seconds > 0 ? static_cast<int64_t>(seconds) * 1000 : 0);
}

GoLiveCardAction GoLiveCardDecide(const GoLiveCardClock& c, int64_t nowMs, bool live, bool livePanelBusy,
                                  int resendMs, GoLiveCardStop* why) {
  GoLiveCardStop stop = GoLiveCardStop::None;
  if (!live) stop = GoLiveCardStop::NotLive;
  else if (livePanelBusy) stop = GoLiveCardStop::LivePanel;
  else if (c.freezeEnded) stop = GoLiveCardStop::FreezeEnd;
  else if (c.untilMs <= c.startMs || nowMs >= c.untilMs) stop = GoLiveCardStop::TimeUp;
  if (stop != GoLiveCardStop::None) {
    // Before the start only the reasons that make the card pointless end it; a not-yet-live
    // mode (the go-live restart still pending) is waited out.
    if (nowMs < c.startMs && stop == GoLiveCardStop::NotLive) return GoLiveCardAction::Wait;
    if (why) *why = stop;
    return GoLiveCardAction::Stop;
  }
  if (nowMs < c.startMs) return GoLiveCardAction::Wait;
  if (c.lastSendMs >= 0 && nowMs - c.lastSendMs < (resendMs > 0 ? resendMs : 0)) return GoLiveCardAction::Wait;
  return GoLiveCardAction::Send;
}

}  // namespace readyup
