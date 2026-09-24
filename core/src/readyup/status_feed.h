#pragma once
// Local status endpoint (docs/FLEET.md §17): the core side.
//
// Owns the Hub + StatusServer (status_snapshot.h / status_server.h), the discovery file
// `csgo/readyup/status.json` and the game-thread collector. The collector runs from the
// GameFrame hook at most every 250 ms: it reads mode / match context / scores / roster / pause /
// knife / stats team lines / plugins / fleet status (`readyup.fleet.v1`, fleet_iface.h), builds
// a StatusInputs and hands it to the HTTP thread with one pointer swap. The HTTP thread never
// calls back into the core, so a slow or hostile client cannot cost a frame.
//
// readyup.cfg (core keys):
//   status_http_enabled = 1          env READYUP_STATUS_HTTP=0 turns it off
//   status_http_bind    = 127.0.0.1
//   status_http_port    = 0          0 = game port + 7 (27022 for 27015); env READYUP_STATUS_HTTP_PORT
//   status_http_token   =            empty = generated once (rst_...) and kept in status.json
//   status_http_metrics = 0          /metrics (Prometheus text)
#include "readyup/selftest.h"

#include <string>
#include <vector>

namespace readyup::status_feed {

// Once, from the shim constructor (also when Ready Up is disabled, so /health says so).
void StartAtLoad();
// Every GameFrame (simulating or not), game thread.
void FrameTick(bool simulating);
// RunSelftest() reports here (any thread).
void NoteSelftest(const SelftestResult& r);
// `ru status_http`: address, token hint, counters.
std::vector<std::string> StatusLines();

}  // namespace readyup::status_feed
