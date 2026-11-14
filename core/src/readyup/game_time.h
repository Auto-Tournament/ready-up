// The server's game time (CGlobalVars curtime).
#pragma once

namespace readyup::game_time {

// Finds the CGlobalVars pointer through ISource2Server::SetGlobals (engine-surface vtable_indices).
// Once, with the Source2Server interface; nothing happens if the slot doesn't verify.
void Init(void* source2Server);

// Every simulating frame: curtime must advance by about one tick at a time before Now() trusts it.
void Frame();

// curtime in seconds, or NaN while it isn't found or hasn't proven itself.
double Now();

}  // namespace readyup::game_time
