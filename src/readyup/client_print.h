#pragma once

namespace readyup {

// Uses the same engine function CounterStrikeSharp uses (UTIL_ClientPrintAll)
// to print to in-game chat without the "Console:" prefix.
//
// Returns true if sent, false if unavailable.
bool ClientPrintAllChat(const char* msg);

// Best-effort print to a specific slot (CounterStrikeSharp's ClientPrint).
// Returns true if sent, false if unavailable.
bool ClientPrintChat(int slot, const char* msg);

// True if UTIL_ClientPrintAll resolved (chat without the "Console:" prefix).
bool ClientPrintAvailable();

}  // namespace readyup

