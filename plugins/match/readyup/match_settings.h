#pragma once

// The engine side of the server settings (server_settings.h): the console commands, chat and
// fleet setters, state.json persistence, and what the settings do each tick.
//
//   console / RCON   ru_<setting>                 show the value, where it comes from, what it does
//                    ru_<setting> <value>         set it (saved in state.json)
//                    ru_<setting> default         back to readyup.cfg / the built-in default
//   chat (admins)    .ru settings show            every setting (also .settings, any player)
//                    .ru settings set <s> <v>     set one (also the ru_series_end_kick_delay_* ones)
//                    .ru settings default <s>
//                    .readyrequired <n> | .playout [on|off] | .roundknife [on|off] | .whitelist [on|off]
//   fleet            cmd settings.set {settings: {<setting>: value, ...}}; server.config
//                    hostname_format and series_end_kick_delay (fleet_bridge.cpp)
//
// Each tick: auto-ready (autoready_enabled), kicks while no match is loaded
// (kick_when_no_match_loaded) and the hostname (hostname_format; the operator's own hostname,
// seen on the console, comes back when the match unloads).

#include "readyup/webhook.h"

#include <string>
#include <vector>

namespace readyup::match_settings {

// Plugin load (after local_store::Init): the persist hook and the saved runtime values.
void Install();

// `ru_<setting> ...` console lines. False when `line` is not one of them.
bool ConsoleCommand(const std::string& line);
const std::vector<std::string>& ConsoleCommands();

// Chat / fleet: `name` is a setting (server_settings.h) or one of the other console settings
// (`series_end_kick_delay_no_demo`, ... with or without `ru_`), run as its console command.
// *reply: the answer ("playout_enabled_default = on" or the error). `by` is logged.
bool Set(const std::string& name, const std::string& value, const std::string& by, std::string* reply);
// Set's checks without changing anything (fleet settings.set: all or none).
bool Validate(const std::string& name, const std::string& value, std::string* err);
bool SetDefault(const std::string& name, const std::string& by, std::string* reply);

// `.settings` / `ru settings show`: every setting, pause_after_restore (round_restore.h), the
// series-end kick delays and what the loaded match sets for itself.
std::vector<std::string> ShowLines();

// Effective values (the match's own value first).
bool PlayoutOn(const WebhookMatchContext* ctx);
bool WhitelistOn(const WebhookMatchContext& ctx);
bool AutoreadyOn(const WebhookMatchContext& ctx);
bool ResetCvarsOnSeriesEnd();
bool PauseCommandIsTactical();
// Knife round for scrims and for match maps without a side (never under the valve ruleset).
bool KnifeDefault(const WebhookMatchContext* ctx);
// Pads map_sides to the map list with the knife default ("knife" or "team1_ct"); match load.
void FillMissingSides(WebhookMatchContext* ctx);

// `hostname "<x>"` seen on the console (server.cfg, an operator): the hostname to restore.
void ObserveHostname(const std::string& value);

// Game thread, every tick (throttled inside).
void Tick(double now);

}  // namespace readyup::match_settings
