#pragma once

// One-shot timers that run on the game thread (GameFrame), outside any Ready Up lock.
// Used by the map-end / series-end flow and the demo recorder.

#include <functional>

namespace readyup {

// Any thread. fn runs on the game thread from GameTimersFrameTick() once `seconds` passed
// (0 = next frame). Timers due in the same frame run in due/scheduling order.
void ScheduleOnGameThread(double seconds, std::function<void()> fn);

// Game thread (game_frame_hook.cpp), every frame, simulating or not.
void GameTimersFrameTick();

}  // namespace readyup
