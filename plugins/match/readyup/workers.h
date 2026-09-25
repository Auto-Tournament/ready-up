#pragma once

// Worker threads of readyup-match.
//
// A plugin must join every thread it started before readyup_plugin_unload returns: after
// that the core dlcloses the image and a thread still running its code crashes the server.
// When the match flow lived in the core it used detached fire-and-forget threads (settings writes,
// webhook sender, admin refresh, demo upload); they all go through here now:
//
//   - Spawn() starts a tracked thread (refused once shutdown began),
//   - long-running loops sleep with SleepFor() and leave when it returns false,
//   - blocking I/O is bounded (HTTP: 3 s connect / 8 s total; local file writes; the
//     demo upload aborts through its progress callback when ShuttingDown()),
//   - Shutdown() (unload) wakes every sleeper and joins every thread.

#include <chrono>
#include <functional>

namespace readyup::workers {

// Starts fn on a tracked thread. False (fn not run) once Shutdown() began.
bool Spawn(const char* what, std::function<void()> fn);

// True once Shutdown() began (unload). Long operations check it and give up.
bool ShuttingDown();

// Sleeps up to `d`; returns false right away (or early) when shutting down.
bool SleepFor(std::chrono::milliseconds d);

// Wakes SleepFor() sleepers without shutting down (e.g. new work for a sender loop).
void WakeAll();

// fn runs when Shutdown() begins, after ShuttingDown() turned true: wake a thread that waits on
// its own condition variable (webhook sender, admin refresh) so it can leave.
void AddWaker(std::function<void()> fn);

// Unload: flags shutdown, wakes sleepers and joins every thread. Logs threads that took long.
void Shutdown();

// Load: clears the shutdown flag (a fresh image after `ru plugin reload match`). The first call
// also registers an exit handler that runs Shutdown() when the server quits without unloading
// the plugin, before this image's static destructors tear down the state the workers use.
void Start();

}  // namespace readyup::workers
