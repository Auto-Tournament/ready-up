#include "readyup/center_html.h"

#include "readyup/config.h"
#include "readyup/engine_surface.h"
#include "readyup/game_events.h"
#include "readyup/logging.h"
#include "readyup/sdk/igameevents.h"

#include <algorithm>
#include <string>

namespace readyup {

// Interface mirrors come from readyup/sdk/igameevents.h (external linkage; see the note
// there). The event manager is the RTTI-verified CGameEventManager from game_events.cpp;
// the old engine2 CreateInterface("GAMEEVENTSMANAGER00x") lookup returned an unverified
// object and has been removed.
using sdk::CKV3MemberName;
using sdk::CPlayerSlot;
using sdk::IGameEvent;
using sdk::IGameEventManager2;

bool PrintCenterHtmlToSlot(int slot, const std::string& html, int durationSeconds) {
  if (slot < 0) return false;
  if (html.empty()) return false;
  IGameEventManager2* mgr = GameEventManagerVerified();
  if (!mgr) {
    if (DebugEnabled()) PrintLine("centerhtml: event manager unavailable (not RTTI-verified); skipping");
    return false;
  }

  durationSeconds = std::clamp(durationSeconds, 1, 10);

  IGameEvent* ev = mgr->CreateEvent("show_survival_respawn_status", /*bForce=*/true, /*cookie=*/nullptr);
  if (!ev) return false;

  // The event expects:
  // - duration: int/long
  // - loc_token: HTML string
  // - userid: player (we provide CPlayerSlot; engine resolves controller/pawn)
  ev->SetInt(CKV3MemberName("duration"), durationSeconds);
  ev->SetString(CKV3MemberName("loc_token"), html.c_str());
  ev->SetPlayer(CKV3MemberName("userid"), CPlayerSlot{slot});

  const bool ok = mgr->FireEvent(ev, /*dontBroadcast=*/false);
  if (!ok) {
    // FireEvent normally frees the event; only free if not fired.
    mgr->FreeEvent(ev);
  }
  return ok;
}

// server: IGameEventListener2* GetLegacyGameEventListener(CPlayerSlot slot).
// Resolved through engine-surface.json ("LegacyGameEventListener"): unique signature +
// global_string identity anchor. On 1.41.8.3 it is a leaf returning
// &gameSystem->proxies[slot] (nullptr if the system is gone or slot > 63).
using GetLegacyGameEventListenerFn = sdk::IGameEventListener2* (*)(CPlayerSlot slot);

bool PrintCenterHtmlToClientOnly(int slot, const std::string& html, int durationSeconds) {
  if (slot < 0 || slot > 63 || html.empty()) return false;

  IGameEventManager2* mgr = GameEventManagerVerified();
  if (!mgr) return false;

  auto getListener = reinterpret_cast<GetLegacyGameEventListenerFn>(EngineFunction("LegacyGameEventListener"));
  if (!getListener) return false;

  sdk::IGameEventListener2* listener = getListener(CPlayerSlot{slot});
  if (!listener) return false;
  // Identity check before the virtual call: the object must be the server's per-client
  // legacy event proxy (vtable RTTI), not whatever a drifted signature happens to return.
  if (!ObjectHasEngineRtti(listener, "CServerSideClient_GameEventLegacyProxy")) {
    static bool s_warned = false;
    if (!s_warned) {
      s_warned = true;
      PrintLine("centerhtml: LegacyGameEventListener returned an object with unexpected RTTI; per-client center HTML disabled.");
    }
    return false;
  }

  durationSeconds = std::clamp(durationSeconds, 1, 10);

  IGameEvent* ev = mgr->CreateEvent("show_survival_respawn_status", /*bForce=*/true, /*cookie=*/nullptr);
  if (!ev) return false;

  ev->SetInt(CKV3MemberName("duration"), durationSeconds);
  ev->SetString(CKV3MemberName("loc_token"), html.c_str());
  ev->SetPlayer(CKV3MemberName("userid"), CPlayerSlot{slot});

  // The proxy serialises the event into this client's net channel synchronously
  // (CounterStrikeSharp FireEventToClient path); we still own the event afterwards.
  listener->FireGameEvent(ev);
  mgr->FreeEvent(ev);
  return true;
}

}  // namespace readyup
