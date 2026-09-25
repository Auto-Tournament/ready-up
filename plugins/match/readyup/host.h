#pragma once

// readyup-match's connection to the core: the ru_api table and a game-thread work queue.
//
// The match flow sources came out of the core and still call what used to be core functions
// (Print, SendToChat, EnqueueServerCommand, ListHumans, FeatureEnabled, ...). Those are now
// small adapters in this plugin (host.cpp, same names and headers as before) that go through
// ru_api. Most of ru_api is game-thread only; adapters called from one of this plugin's worker
// threads (webhooks, DB, recovery) queue the call and run it on the next frame instead.

#include "readyup/plugin_api.h"

#include <cstdint>
#include <functional>

namespace readyup::host {

// The table the core handed to readyup_plugin_load; nullptr before Attach / after Detach.
const ru_api* Api();

// readyup_plugin_load (game thread): remembers the table and the game thread.
void Attach(const ru_api* api);
// readyup_plugin_unload, after every worker thread was joined: drops the table and the queue.
void Detach();

bool OnGameThread();

// Runs fn on the game thread: right away when called there, otherwise from the next frame
// (DrainGameThreadQueue). Dropped after Detach.
void RunOnGameThread(std::function<void()> fn);

// on_frame: runs queued work, refreshes the per-frame caches off-thread callers read.
void FrameBegin();

// Seconds (CLOCK_MONOTONIC) of the current frame, for code that used steady_clock deltas.
double NowSeconds();

}  // namespace readyup::host
