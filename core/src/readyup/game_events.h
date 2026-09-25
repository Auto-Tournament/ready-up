#pragma once

#include <optional>
#include <string>
#include <utility>

namespace readyup {

// Locates and RTTI-verifies the engine's CGameEventManager (idempotent, any thread).
// Makes no virtual calls into the manager; registration happens in GameEventsFrameTick().
void InstallGameEventsListener();

// Game thread only (called from GameFrame via Tick): registers the listener once the
// manager has loaded its event descriptors, confirms it with FindListener, and re-registers
// if the registration is ever lost. Throttled internally.
void GameEventsFrameTick();

// True once the listener is registered AND the engine has delivered at least one event.
bool GameEventsListenerInstalled();

// Lets the game-events module observe engine/server interface pointers as they
// are created (from our CreateInterface wrapper). This improves reliability by
// avoiding creating extra interface instances and lets us retry resolution once
// the owning interface is available.
void GameEventsObserveInterface(const char* name, void* iface);

// Best-effort: returns current CS team number for a slot (2=T, 3=CT), or nullopt if unknown.
std::optional<int> GetCsTeamNumForSlot(int slot);

// Records a slot's team as seen in server log lines ("Name<slot><id><CT>" or
// "switched from team <X> to <Y>"). GetCsTeamNumForSlot falls back to this
// when engine events are unavailable. team: 0 unassigned, 1 spec, 2 T, 3 CT.
void ObserveSlotTeamFromLog(int slot, int team);

// Last CCSPlayerController* seen for a slot in engine events (nullptr if unknown).
void* GameEventsControllerForSlot(int slot);

// Engine slot of a connected player (a controller was seen for it), or nullopt.
std::optional<int> GameEventsSlotForSteam(unsigned long long steamid64);

// CGameEventManager availability: 0 = not yet (waiting for Init on map load), 1 = located and
// RTTI-verified, 2 = unavailable (Init unresolved / RTTI mismatch).
int GameEventManagerStatus(std::string* detail);

// True if the CGameEventManager::Init detour (fallback capture path) is installed.
bool GameEventsInitHookInstalled();

// For `ru selftest`.
struct GameEventsStatus {
  bool manager = false;             // verified CGameEventManager
  bool listenerRegistered = false;  // AddListener succeeded for the core events
  bool delivered = false;           // at least one event arrived
  unsigned long long count = 0;     // events delivered so far
  std::string lastEvent;
};
GameEventsStatus GetGameEventsStatus();

namespace sdk {
class IGameEventManager2;
}
// The engine's CGameEventManager, or nullptr unless it was located and its vtable RTTI
// verified (and is unchanged). Never call into an event manager obtained any other way.
sdk::IGameEventManager2* GameEventManagerVerified();

}  // namespace readyup
