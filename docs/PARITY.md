# Ready Up vs Auto Tournament CS2: parity matrix

This document lists what Ready Up must do before it can replace the Auto Tournament CS2 plugin (formerly MatchZy Enhanced) on Auto Tournament servers.

**Decision (Sivert, 2026-09):** there will be no adapter. The platform will change its `at_` prefix to `ru_`. Ready Up implements the AT CS2 plugin's platform contract **1:1**, with only the prefix changed. That means the same command names after the prefix, the same arguments, the same RCON reply strings the platform parses, the same status values, the same event names and payloads, and the same HTTP headers. The `css_*` and `get5_*` names the platform sends keep working unchanged. Ready Up's own names (`ru_webhook_url`, `ru_match_token`, `ru_heartbeat_url`, `ru match load`, `.ru ...`) stay as extras.

Sources compared:

| Codebase | Version | Path prefix used below |
|---|---|---|
| Ready Up | `Auto-Tournament/ready-up` master `490ed3a` | `src/readyup/` → **RU:** |
| AT CS2 plugin | `Auto-Tournament/cs2-plugin` `f9e2af8` | `src/` → **AT:** |
| Platform | `Auto-Tournament/auto-tournament` main `2004eee` | `api/src/integrations/cs2/` → **P:** |
| Plugin docs | `Auto-Tournament/docs` `content/docs/cs2/plugin/*.mdx` | (the docs still use the old `matchzy_*` names; the code uses `at_*`) |

Status: **done** = matches the contract, **partial** = the feature exists but the name, arguments, reply or payload differ, **missing** = not implemented, **not needed** = the platform does not depend on it.
Effort: **S** < 1 day, **M** 1–3 days, **L** > 3 days.
Engine column: **none** = logic, HTTP or built-in console commands/cvars only. **events** = needs engine game events, which Ready Up already hooks (the `Events` feature). **fragile** = needs new signatures, offsets or schema writes; avoid these where possible.

---

## 0. Cross-cutting findings (read these first)

1. **Auth header mismatch (P0).** Ready Up sends `Authorization: Bearer <ru_match_token>` on webhooks, heartbeats and the match-config GET (RU: `webhook.cpp` `HttpPostJson(url, token, …)`, `command_buffer_hook.cpp` `LoadMatchFromUrl`). The platform only accepts `X-Auto-Tournament-Token` (`api/src/middleware/serverAuth.ts:55`, P: `utils/pluginRconCommands.ts:116`). Every Ready Up request is rejected today. Ready Up needs a configurable header key and value per channel: remote log, match load, demo upload, report, bootstrap.
2. **Event URL mismatch (P0).** Ready Up posts to `<ru_webhook_url>/<slug|matchid|"unknown">` (RU: `webhook.cpp` `EndpointForLocked`). The platform sets the full URL `<base>/api/events?server_id=<id>` and the AT plugin posts to it as-is. With the query string, Ready Up would build `…/api/events?server_id=x/r1m1`. `ru_remote_log_url` must be used verbatim.
3. **Console commands are text-intercepted, not registered.** Ready Up handles console input by patching `CCommandBuffer::AddText` through the GOT (RU: `command_buffer_hook.cpp` `Hook_AddText`) and matching the first line. That is fine for the new `ru_*` commands and the `css_*` aliases, because none of them need to be real ConVars. Two things still need checking:
   - The platform parses **RCON replies**: `queued_match=`, `cleared_queued_match=`, `successfully`, `"at_tournament_status" = "idle"`. Anything Ready Up prints from inside the hook must reach the RCON response buffer. `ru sigtest` over RCON suggests it does. Add an RCON test for every reply string the platform parses.
   - A status query (`ru_tournament_status` with no argument) must print `"ru_tournament_status" = "<value>"`, which is the format the platform parser accepts. Ready Up has to print this itself, because there is no ConVar behind the name.
4. **Config cvars reach Ready Up as console commands.** The platform applies every stored config cvar as `<k> <v>` over RCON, and the match JSON `cvars{}` does the same (RU: `modes.cpp` `ApplyMatchCvarsLocked`). After the rename those keys are `ru_*`. Each one must be recognised (and consumed) by the AddText hook, or it reaches the engine as "Unknown command". The strict `IsSafeConvarValue` filter also drops values with spaces or quotes, such as `ru_hostname_format "{TEAM1} vs {TEAM2}"`. Quoted values need handling.
5. **Match-config HTTP GET runs synchronously on the game thread** (RU: `command_buffer_hook.cpp` `LoadMatchFromUrl`, comment "currently synchronous on the server thread"). The same will be true for bootstrap and backup-URL downloads unless they move to the sender thread. This is a hitch risk on a live server with a slow platform. Make them async (M).
6. **Series state is memory-only.** The maps won (`seriesWinsTeam1/2` in RU: `modes.cpp`) are not persisted. If the server restarts during a Bo3, the series score is lost (RU: `match_recovery.cpp` restores the config, the round backup and the live flag only).
7. **Architecture.** `docs/ARCHITECTURE.md` plans to split the match logic into `plugins/match.so`. Everything below except the engine-flagged items is policy code, so it belongs in the match plugin. Doing the parity work before the migration (step 4) means moving it twice. Doing it after means waiting 2–2.5 weeks. Recommendation: implement the P0 items now in `src/readyup/` behind small, self-contained files (`at_contract_*.cpp`), so step 4 can move them whole.

---

## 1. Server setup: bootstrap, webhook, report

Sent by the platform when a server is added or re-initialised. P: `utils/pluginRconCommands.ts:11-62`, `routes/serverBootstrap.ts:21`.

| Platform sends (`at_` → `ru_`) | AT reference | Ready Up today | Status | Needed | Engine | Effort |
|---|---|---|---|---|---|---|
| `ru_clear_event_queue` | AT: `ConsoleCommands.cs:1140` | Queue in memory, no clear (RU: `webhook.cpp` `St().q`) | missing | Drop the queue. Reply like AT. | none | S |
| `ru_server_id "<id>"` | AT: `BootstrapFetchDebouncer.cs:217` | Identity is derived from the match slug. There is no server id. | missing | Store and persist it. Use it in `server_configured`/`server_health`/`test_event` `server_id` and in the report `serverId`. | none | S |
| `ru_bootstrap_token "<token>"` | AT: `BootstrapConfig.cs:78` | none | missing | Persist the token. Send it as `X-Auto-Tournament-Token` on the bootstrap GET. | none | S |
| `ru_bootstrap_url "<base>/api/servers/<id>/bootstrap"` | AT: `BootstrapConfig.cs:58`, debounce 1.5 s | none | missing | Debounced async GET → `{success, serverId, commands[]}`. Run each command through the AddText path, persist them, and re-fetch on boot. Warn if the `serverId` differs. | none | M |
| `ru_remote_log_url "<base>/api/events?server_id=<id>"` | AT: `RemoteLogConfig.cs:11-83`, `ConsoleCommands.cs:1009` | `ru_webhook_url` (appends `/<slug>`) | partial | Add a `ru_remote_log_url` alias that posts to the URL verbatim (finding 2). Persist it. Emit `server_configured` when it is set. | none | S |
| `ru_remote_log_header_key "X-Auto-Tournament-Token"` / `ru_remote_log_header_value "<token>"` | AT: `RemoteLogConfig.cs` | Bearer only | missing | Custom header on event POSTs (finding 1). | none | S |
| `ru_report_endpoint "<base>/api/events/report"` / `ru_report_token` / `ru_report_server_id` | AT: `ConfigConvars.cs:157-159`, `MatchReportCommand.cs` | none | missing | See §6, match report. | none | S (settings) |
| `get5_check_auths true` | AT: get5 alias | none | missing | Accept it (Ready Up always checks SteamIDs), reply OK, no-op. | none | S |
| Core settings: `ru_chat_prefix`, `ru_admin_chat_prefix`, `ru_knife_enabled_default`, `ru_debug_chat` | AT: `ConfigConvars.cs:386`… | Chat prefixes come from `readyup.cfg` (`ChatPrefix()`, `AdminPrefix()`). Knife is per match through `map_sides`. | partial | Map to the existing config keys at runtime and persist them. `knife_enabled_default` applies when `map_sides` is absent. | none | S |
| `ru_config_scope` (`+ru_config_scope` launch arg) | AT: `PersistentConfigStore.cs`, `ServerIdentity.cs` | `persisted_settings.cpp` (file per install) | partial | Needed only if several servers share one data dir or DB. Scope the persisted settings file and DB rows by it. | none | S |

## 2. Per-server settings (bootstrap `commands[]`)

P: `utils/pluginRconCommands.ts:184-305`. All of them are `<name> <value>` and must be accepted and persisted.

| Setting (`ru_…`) | AT ref | Ready Up today | Status | Needed | Engine | Effort |
|---|---|---|---|---|---|---|
| `ru_minimum_ready_required <n>` | `ConfigConvars.cs:258` | Needs **every** roster player connected and ready (RU: `modes.cpp` `AllRosterReadyAndConnectedLocked`) | partial | Go-live threshold per team (0 = all). **A roster with substitutes can never go live today.** | none | S |
| `ru_allow_force_ready 0\|1` | `ConfigConvars.cs` | `allow_force_ready` (match config / readyup.cfg, default on; RU: `match_rules.h`) | done | – | none | S |
| `ru_pause_after_restore 0\|1` | `ConfigConvars.cs` | Recovery always pauses | partial | Make the pause after restore conditional. | none | S |
| `ru_stop_command_available`, `ru_stop_command_no_damage` | `BackupManagement.cs:39` | no `.stop` | missing | See §7. | none | M |
| `ru_whitelist_enabled_default 0\|1` | `ConfigConvars.cs` | Whitelist is always on when a match is loaded (RU: `modes.cpp` `EnforceWhitelistLocked`) | partial | Make it a toggle (also `.whitelist`). | none | S |
| `ru_kick_when_no_match_loaded 0\|1` | `ConfigConvars.cs` | Scrim flow when idle | missing | Kick non-admins when no match is loaded and the setting is on. | none | S |
| `ru_playout_enabled_default` | `ConfigConvars.cs` | none | missing | Play all rounds and ignore clinch (RU: `DetermineMapWinnerIfComplete`). | none | S |
| `ru_reset_cvars_on_series_end` | `ConfigConvars.cs` | Always resets to idle (RU: `ResetServerRulesAndRestartLocked`) | partial | Honour the flag. | none | S |
| `ru_use_pause_command_for_tactical_pause` | `ConfigConvars.cs` | `.pause` = `mp_pause_match` | partial | When 0, `.pause` calls a tactical timeout (`timeout_ct_start`/`timeout_terrorist_start`). | none | S |
| `ru_autostart_mode 0\|1\|2` | `ConfigConvars.cs:482` | Scrim flow when idle, no mode switch | partial | Map 0/1/2 to idle / warmup (auto scrim) / other, and publish `warmup` status for an idle server in warmup (AT: `TournamentStatusLogic.cs:44-47`). | none | S |
| `ru_hostname_format "<fmt>"` (empty = off) | `ConfigConvars.cs:75` | none | missing | Set `hostname` from `{TEAM1}`/`{TEAM2}`/… on load and reset. Needs quoted-value handling (finding 4). | none | S |
| `ru_demo_path`, `ru_demo_name_format` | `ConfigConvars.cs`, `DemoManagement.cs` | Fixed name `ru_<slug>_m<N>_<map>_<ts>` in the GOTV default dir (RU: `modes.cpp` `StartDemoForMapLocked`) | partial | Apply path and name tokens. | none | S |
| `ru_series_end_kick_delay_no_demo`, `_demo_no_upload`, `_demo_upload` | `ConfigConvars.cs:164` | Nobody is kicked at series end | missing | Kick all players after the delay that fits the demo outcome. This frees the server for turnover. | none | S |
| `ru_demo_upload_url` | `ConfigConvars.cs:309` | none | missing | See §5. | none | – |

## 3. Match load and lifecycle commands

P: `services/matchLoadingService.ts`, `allocation.ts`, `routes/rcon.ts`.

| Platform sends | AT ref | Ready Up today | Status | Needed | Engine | Effort |
|---|---|---|---|---|---|---|
| `ru_loadmatch_url "<url>" "X-Auto-Tournament-Token" "<token>"` (URL has `?server_id=&match_id=`, 409 on mismatch) | AT: `MatchManagement.cs:99-175` | `ru match load <url>` with a Bearer token (RU: `command_buffer_hook.cpp` `LoadMatchFromUrl`) | partial | New command. Quoted args plus an optional header pair. Load async (finding 5). Validate the required fields (`maplist`, `num_maps`, `team1/2.name/players`) like AT. | none | M |
| Reply strings the platform classifies: `queued_match=<id>` / "queued next match" / "to load after reset"; `gotv[0] not active`; "cannot load a new match" / "already setup"; "match load failed" | AT: `MatchManagement.cs:189, 316` | Prints `match-load[n]: …` and **replaces** any loaded match (it emits `series_end` "none" for the old one) | missing | **Queue semantics**: if status is `postgame`/`queued`, queue the load, reply `queued_match=<slug>`, and load after the series reset. Otherwise load and reply in AT's wording. On failure print `match load failed: <reason>`. | none | M |
| `ru_clear_queued_match` → `cleared_queued_match=<id\|none>` | AT: `MatchManagement.cs:244-260` | none | missing | Comes with the queue. | none | S |
| `ru_loadmatch <file>` | AT: `MatchManagement.cs:58` | none | missing | Read from `csgo/`. | none | S |
| `ru_addplayer <steam64> <team1\|team2\|spec> "<name>"`; the platform looks for "successfully" | AT: `Teams.cs:72-112` | none (the roster is fixed at load) | missing | Change the roster at runtime and update the whitelist/team enforcement. Reply "…successfully…" or AT's error strings. | none | S |
| `ru_removeplayer <steam64>` | AT docs | none | missing | | none | S |
| `css_endmatch` (end and reset; used for allocation) | AT: `ConsoleCommands.cs:568` | `ru match end` | partial | Alias, plus `get5_endmatch`/`css_forceend`. Like AT, it must also clear the queued match (reply `cleared_queued_match=…`) and reset to idle, so status becomes allocatable (AT: `ConsoleCommands.cs:568-580`). | none | S |
| `css_restart` (restart / reallocate / reset) | AT: `ConsoleCommands.cs:594` | `ru match restart` (back to warmup, match kept) | partial | In AT, `css_restart`/`css_rr` do the **same as `css_endmatch`**: `ClearQueuedMatch` + `ResetMatch()` unloads the match (AT: `ConsoleCommands.cs:593-606`). Ready Up's `ru match restart` keeps the match (`ru map restart` is `mp_restartgame 1`) and returns to warmup, so `css_restart` must **not** alias it. Alias it to the end/reset path instead. | none | S |
| `css_start` | AT: `ConsoleCommands.cs:641` | `ru match start` | partial | Alias. | none | S |
| `css_map <m>` | AT: `ConsoleCommands.cs:617` | none | missing | Pre-live `changelevel`/`host_workshop_map`. | none | S |
| `css_pause` / `css_unpause` / `css_forcepause` / `css_forceunpause` | AT: `ConsoleCommands.cs:264-278` | `ru match pause` / `ru match unpause` (admin) | partial | Aliases. | none | S |
| `css_restore <n>` | AT: `BackupManagement.cs:116` | `ru match recover [n]` only **emits `recover_requested`**. Nothing is restored except the boot recovery. | missing | See §7. | none | M |
| `css_switch` (swap teams) | AT: `ConsoleCommands.cs:184` | none | missing | `mp_swapteams` and keep the team1/team2 side mapping in sync. | none | S |
| `css_prac` / `css_exitprac` | AT: `PracticeMode.cs:775` | `ru mode practice` / `ru mode idle` | partial | Aliases. | none | S |
| `css_asay <msg>` | AT: `AutoTournamentCS2.cs:777` | none (`say` works) | missing | Chat with the admin prefix. | none | S |
| `reload_admins` / `css_reload_admins` | AT | `ru_admins_url` + `ru admins` (admins.json/MAT) | partial | Alias to a MAT admins refresh (RU: `mat_admins::RefreshNow`). | none | S |
| `css_skipveto` | not registered in AT | none | not needed | The platform runs the veto in the browser (`skip_veto: true`). Accept it as a no-op. | none | S |
| catalog: `css_roundknife`, `css_playout`, `css_whitelist`, `css_settings`, `css_readyrequired <n>`, `css_team1/2 <name>` | AT: `ConsoleCommands.cs` | none | missing | Toggles over the §2 settings. `team1/2` sets the names (plus `mp_teamname_1/2`). | none | S |
| Plain engine commands (`mp_restartgame 1`, `mp_warmup_end`, `mp_roundtime_defuse`, `say`) | – | pass through to the engine | done | Note: `mp_warmup_end` during Ready Up's own warmup can confuse the gating. Map "end warmup" to `ru match start`. | none | S |

## 4. Status convars read over RCON

P: `services/serverStatusService.ts:31`, `allocation.ts:386`. Parser accepts `"n" = "v"` and `n = v`.

| Name (`ru_…`) | AT ref | Ready Up today | Status | Needed | Effort |
|---|---|---|---|---|---|
| `ru_tournament_status` | AT: `TournamentStatusLogic.cs`, go-live value `playing` (`ConfigConvars.cs:154`) | Heartbeat JSON `status` only: `idle\|loading\|warmup\|live\|postgame\|error` (RU: `webhook.cpp` `HbStatusToString`) | missing | Queryable value with the AT enum: `idle, loading, warmup, knife, playing, paused, halftime, postgame, queued, error`. AT publishes `playing` when a match goes live. The platform enum names that state `live` (P: `services/serverStatusService.ts:10-21`) but only uses idle/warmup/error for allocation, so either value works. Publish `playing` to match AT 1:1. Idle-in-warmup with no match = `warmup` (allocatable). Set it on every transition, including pause, halftime and knife. | S |
| `ru_tournament_match` | AT: `SafeAutoUpdater.cs:48` | none | missing | Loaded match slug. Empty when idle (AT: `TournamentStatusLogic.cs:37-40`). | S |
| `ru_tournament_next_match` | AT: `ConfigConvars.cs:153` | none | missing | Queued slug. | S |
| `ru_tournament_updated` | AT: `ConfigConvars.cs:152` | none | missing | Unix time of the last change. | S |
| `version`, `status` (CS2 build) | engine | engine | done | – | – |

## 5. Demos

P: `routes/demos.ts:40`; `matchLoadingService.ts:213` sets the URL per match when `cvars.ru_demo_recording_enabled != 0`.

| Item | AT ref | Ready Up today | Status | Needed | Engine | Effort |
|---|---|---|---|---|---|---|
| Recording per map (`tv_enable 1`, `tv_record`, stop at map end) | AT: `DemoManagement.cs` | done (RU: `modes.cpp` `StartDemoForMapLocked` / `StopDemoLocked`) | done | Honour `ru_demo_recording_enabled`, path and name format. | none | S |
| Upload: `POST ru_demo_upload_url`, `application/octet-stream`, ≤500 MB, headers `ru_demo_upload_header_key/_value` (= `X-Auto-Tournament-Token`) + `Auto-Tournament-FileName`, `-MatchId`, `-MapNumber`, `-RoundNumber` (Get5-* fallback accepted) | AT: `DemoManagement.cs:275-285`, `DemoFileLocator.cs` | **none** | missing | After `tv_stoprecord`, wait for the file to finish (size stable), locate it under `csgo/<demo_path>`, and stream-upload it from a worker thread (libcurl is already linked). **Keep the `Auto-Tournament-*` header names**: the platform owns them. | none | M |
| Events `demo_recording_start`, `demo_recording_stop`, `demo_upload_start`, `demo_upload_success`, `demo_upload_fail`, `demo_upload_ended` | AT: `Events.cs:207-290` | none | missing | The platform uses these to **turn the server over** after a series. Without them, a server with demo recording on waits out the fallback. Payloads per `Events.cs` (`filename`, `size_mb`, `status`, `reason`, `success`). | none | S |

## 6. Events (webhook payloads)

The contract is `AT: Events.cs` plus `MatchData.cs`. The platform normalizer reads `team1.score\|team1_score`, `winner.team\|winner`, and `players[].stats{kills, deaths, assists, kast, headshot_kills, flash_assists, utility_damage, mvp\|mvps, score, rounds_played, damage}`. Send the AT shapes exactly, so the existing normalizer and its tests cover Ready Up unchanged.

| Event | AT payload | Ready Up (RU: `webhook.cpp`) | Status | Gap | Effort |
|---|---|---|---|---|---|
| `server_configured` | `server_id, hostname, plugin_version, remote_log_url, timestamp, configured_by` | same fields. `server_id`/`hostname` = slug or "unknown", `matchid:-1` | partial | Use `ru_server_id` and the real `hostname`. **The platform reads `plugin_version` from this event only**: it will compare it against `cs2-plugin` releases, so point the version check at `ready-up` releases (platform-side, 1 line). | S |
| `server_health` | `server_id, plugin_version, timestamp, db_ok, db_type (sqlite\|mysql), db_error, reason` | `db_type:"readyup"`, `db_ok:true` always | partial | Ready Up has no database (FLEET.md D13); report the JSON store state instead. Emit periodically like AT. | S |
| `test_event` (from `css_te` / `css_testevent`) | AT: `Events.cs:575`, `ConsoleCommands.cs:258, 998` | none | missing | The platform uses it for the connection test. | S |
| `cs2_update_required` | `server_id, required_version, phase, timestamp` | none (the CS2 build is already in the heartbeat, RU: `cs2_version.cpp`) | missing | Steam `UpToDateCheck` HTTP call (appid 730, current `PatchVersion`). | S |
| `series_start` | `team1:{id,name}, team2:{id,name}, num_maps` | `team1_name, team2_name, num_maps` | partial | Emit `team1`/`team2` objects (team `id` from match JSON `team1.id`). | S |
| `going_live`, `warmup_ended`, `knife_round_started`, `halftime_started`, `overtime_started`, `side_swap`, `round_started` | map events | same names and fields | done | – | – |
| `knife_round_ended` | `winner` | `winner` ("team1"/"team2") | done | Check the AT winner type (object vs string) against the normalizer. | S |
| `side_picked` | `map_name, map_number, side, picked_by, team` | same | done | – | – |
| `round_end` | `round_number, round_time, reason, winner:{side,team}, team1/team2:{id,name,series_score,score,score_ct,score_t,players[{steamid,name,stats{…full PlayerStats…}}]}` | flat `winner` string, `team1_score`, `team1.players[].stats{kills,deaths,assists,headshot_kills,damage,mvps,score}`, `round_time:0` | partial | Emit the full AT shape: winner object, team scores inside `team1/team2`, `round_time`, and the full stat set (§8). | M |
| `map_result` | `winner:{side,team}, team1/team2: StatsTeam (with players)` | `map_name, team1_score, team2_score, winner` string, **no players** | partial | Emit the full shape with final player stats. The platform takes final per-map stats from here. | S (after §8) |
| `series_end` | `winner:{side,team}, team1_series_score, team2_series_score, time_until_restore` | `winner` string | partial | Winner object. Put the kick delay in `time_until_restore`. | S |
| `player_connect` / `player_disconnect` | `player:{steamid,name,team}` | same | done | – | – |
| `player_ready` / `player_unready` | `player, team, ready_count_team1/2, total_ready, expected_total` | same | done | – | – |
| `team_ready`, `all_players_ready` | AT: `Events.cs:346-380` | none | missing | Emit them from the ready gate. | S |
| `match_paused` / `unpause_requested` / `match_unpaused` | same | same; `is_tactical` true for `.tac`, `pause_time` = timeout / tech limit seconds, `teams_needed` 1 or 2 by the unpause rule (RU: `match_features.cpp`) | done | – | S |
| `pause_requested` | defined, never sent by AT | none | not needed | – | – |
| `backup_loaded` | `round_number, filename` | none | missing | Emit it on restore (§7). | S |
| `map_picked` / `map_vetoed` | platform-side veto | none | not needed | Veto runs in the browser. | – |
| `player_stats_update` | the platform consumes it; AT does not send it | none | not needed | – | – |
| Ready Up extras: `player_gg`, `match_forfeit`, `recover_requested` | not in AT | sent | extra | The platform ignores them. Either map them to AT behaviour (§7 `.gg`) or keep them as extras. | – |
| Retry queue: AT persists it in the DB, retries every `ru_event_retry_interval` (30 s), gives up after 20 tries | AT: `PublishEvents.cs` | memory queue, exponential backoff 30 s→32 min, 20 tries, **lost on restart** (RU: `webhook.cpp` `SenderThread`) | partial | Persist it (a file is enough). Add `ru_get_pending_events`. | S |
| Heartbeat | AT: `MatHeartbeat.cs` (`x-auto-tournament-token`) | `ru_heartbeat_url` → `/api/servers/:id/heartbeat` every 5 s | not needed | **The platform has no heartbeat route**: it treats any event as liveness (P: `events/routes.ts:325`). Keep it as an extra, or drop it. | – |

### Match report (live match page)

| Item | AT ref | Ready Up | Status | Needed | Effort |
|---|---|---|---|---|---|
| Push `POST ru_report_endpoint` with `x-auto-tournament-token: <ru_report_token>`, body `{serverId, matchSlug, report:{match{matchId,slug,phase,map{name,number,total,round},score}, teams{team1/2{name,side,players[]}}, spectators, connections, server{moduleVersion,tournamentStatus}}}`, on connect/disconnect, warmup start, after knife, round start/end, 3 tries | AT: `MatchReportCommand.cs` | none | missing | Build it from the state Ready Up already has (ready set, pause state, roster, slot registry). | M |
| Pull: `ru_match_report` / `css_match_report` → JSON after the first `{` | AT: `MatchReportCommand.cs:144-145` | `ru match state` (text) | missing | Same builder. Print the JSON on one line. | S |

## 7. Player-facing and admin features

| Feature | AT ref | Ready Up today | Status | Needed | Engine | Effort | Prio |
|---|---|---|---|---|---|---|---|
| `.ready/.r`, `.unready/.ur/.notready` | `ReadySystem.cs` | done (RU: `ru_router.cpp`) | done | `!`-prefixed and `css_ready` console forms, if wanted | none | S | P2 |
| Ready HUD / reminders | – | per-player center HTML (RU: `ready_hud.cpp`) | done (better) | – | – | – | – |
| `.forceready` | `ReadyLogic.cs` | readies the caller's whole team in warmup once `min_players_to_ready` of it is connected (0 = full roster); the ready gate uses the same threshold. Rule `allow_force_ready` (RU: `match_features.cpp`) | done | – | none | S | P1 |
| `.start/.forcestart`, `.restart/.rr`, `.endmatch/.forceend` | `ConsoleCommands.cs` | `.ru match start/restart/end` | partial | Chat aliases | none | S | P2 |
| `.pause/.p` tactical vs `.tech` technical; limits `ru_max_pauses_per_team`, `ru_pause_duration`, `ru_both_teams_unpause_required` (match cvars) | `Pausing.cs`, `ConfigConvars.cs:128-130` | `.pause`/`.p`/`.tech` = technical (`mp_pause_match`), `max_tech_pauses_per_team` per team per map, `tech_pause_max_seconds` auto-unpause with a HUD countdown, `both_teams_unpause_required` (else the pausing team alone); `.tac` = tactical (RU: `match_features.h`, `match_rules.h`) | done | – | none | M | P1 |
| `.tac` tactical timeout | `ConsoleCommands.cs:399` | `timeout_ct_start` / `timeout_terrorist_start` for the caller's side; the engine enforces `mp_team_timeout_max/_time` (fleet `rules.pause.tactical_*` map onto them); shown as a tactical pause until `round_freeze_end` (RU: `match_features.cpp`) | done | – | none (built-in commands) | S | P1 |
| `.forcepause/.fp`, `.forceunpause/.fup` | `ConsoleCommands.cs:278` | chat aliases of `.ru match pause` / `.ru match unpause`; an admin pause only ends with `.fup` | done | – | none | S | P2 |
| Knife round + `.stay/.switch/.swap/.ct/.t` | `MatchLogic.cs` | done (log-driven knife tracker, RU: `knife_tracker.cpp`, `modes.cpp` `ApplyKnifeSideChoiceLocked`) | done | Read `ru_side_selection_enabled` / `ru_side_selection_time` from `cvars{}` (today: `knifeDecisionSeconds`, default 60 s, sides stay on timeout). `.roundknife` toggle. | none | S | P1 |
| `.gg` vote to give up (`ru_gg_enabled`, `_threshold`, `_min_score_diff`) | `ConfigConvars.cs:138` | `.gg` only emits `player_gg` | partial | Team vote → end the map/series with the other team winning (forfeit through `map_result`/`series_end`) | none | S | P2 |
| Forfeit when a team leaves (`ru_ffw_enabled`, `ru_ffw_time`) | `ConfigConvars.cs:143` | `forfeit_after_seconds` (default 240, 0 = off): a team with nobody connected on a live map gets a chat + HUD countdown, cancelled on reconnect; then it forfeits the map and the series through `map_result` / `series_end` (plus `match_forfeit` / fleet `event.forfeit`, reason `team_absent`). `.ff` still only emits `match_forfeit` (RU: `match_features.cpp`, `modes.cpp` `ForfeitCurrentMap`) | done | – | none | S | P1 |
| Auto-ready (`ru_autoready_enabled`) | `ConfigConvars.cs:85` | none | missing | Mark players ready on join | none | S | P2 |
| `.stop` round-restore vote (`ru_stop_command_available`, `_no_damage`) | `BackupManagement.cs:39` | none | missing | Both teams vote → restore the start of the current round | none | S (after restore) | P2 |
| **Round restore** `css_restore <n>` / `.restore <n>`, `ru_loadbackup <file>`, `ru_loadbackup_url <url>`, `ru_listbackups <matchId>`, `ru_remote_backup_url` + header | `BackupManagement.cs:116, 564-642` | CS2 built-in round backups are on (`mp_backup_round_file readyup_backup_<id>_map<N>_`); a restore happens **only** on boot recovery (RU: `match_recovery.cpp`); `ru match recover` only emits an event | partial | Restore by round number: pick the file for round N by prefix, `mp_backup_restore_load_file`, keep the pause, fix Ready Up's round/score/stat state, emit `backup_loaded`. The URL variant downloads to `csgo/` first. Remote backup upload is P2. Prefer the built-in `.txt` backups over AT's custom JSON. | none (built-in cvars) | M | P1 |
| Crash/restart recovery | AT relies on backups + DB | done for the config, round backup and live gate (RU: `match_recovery.cpp`) | partial | Persist the series score (finding 6) and the stats accumulators | none | S | P1 |
| Whitelist (roster + spectators + admins), kick non-roster | `MatchLogic.cs` | done (RU: `modes.cpp` `EnforceWhitelistLocked`) | done | `.whitelist` toggle | none | S | P2 |
| Team enforcement (force `jointeam` by roster/side) | `Teams.cs` | done (RU: `modes.cpp` `MaybeForceRosterTeamsLocked`) | done | – | ClientCommand hook (exists) | – | – |
| Team names in game (`mp_teamname_1/2`, flags) | `MatchManagement.cs` | `mp_teamname_1/2` + `mp_teamflag_1/2` (team `flag`) with every match-cvar apply (warmup, knife pick, go-live): name 1 = the team starting on CT; the engine keeps them across halftime. Scrims keep the default names; reset on unload (RU: `modes.cpp` `AppendTeamNameCmds`) | done | CS2 has no team tag cvar | none (cvars) | S | P1 |
| Coach `.coach ct\|t`, `.uncoach` | `Coach.cs`, `Teams.cs:37` | none | missing | AT moves the coach to the team, then keeps them dead and outside player slots at every round start. Try CS2 `sv_coaching_enabled` + the `coach` client command first. If that is broken, emulate it (`player_spawn` → slay/teleport). That needs pawn/entity writes. | **fragile** if emulated | L | P2 |
| Spectators whitelist | `MatchConfig.cs` | done (`spectators.players`) | done | `min_spectators_to_ready` | none | S | P2 |
| Match-scoped admins (`admins[]`) | `MatchConfig.cs` | done (RU: `match_config_parser.cpp`) | done | – | – | – | – |
| `.help`, `.version`/`.atversion` | – | `.help`, `.ru version` | done | `.ruversion` alias | none | S | P2 |
| `.map <name>` (stock + workshop), `.reloadmap` | `ConsoleCommands.cs:617` | none | missing | See workshop maps below | none | S | P2 |
| `.team1/.team2 <name>`, `.settings`, `.readyrequired <n>`, `.playout`, `.roundknife`, `.whitelist` | `ConsoleCommands.cs` | none | missing | Toggles over the settings above | none | S | P2 |
| `.asay`, `.rcon` | `AutoTournamentCS2.cs:777, 843` | none | missing | `.rcon` goes through AddText | none | S | P2 |
| `.testevent/.te` | `ConsoleCommands.cs:258` | none | missing | Same as `css_te` | none | S | P1 |
| Damage report (per-round `.dmg`/end-of-round print) | `DamageInfo.cs` | Damage is tracked (`player_hurt`, used only for the tiebreak) | missing | Print given/taken per opponent at round end | events | S | P2 |
| Practice: `.prac/.tactics`, `.bot/.cbot/.boost/.nobots` | `PracticeMode.cs` | done (a subset, RU: `ru_router.cpp`) | partial | `.exitprac/.match`. Bots spawn by server command, not at the player's position. | none | S | P2 |
| Practice extras: `.savenade/.loadnade/.rethrow/.noflash/.showspawns/.spawn/.god/.clear/.fastforward/.timer` | `PracticeMode.cs`, `GrenadeProjectiles.cs`, `spawns/` | none | missing | Most need pawn position/angle reads and teleports, plus projectile entity hooks | **fragile** (schema + entity writes, projectile creation) | L | P2 |
| Map veto (in-plugin) | – | none | not needed | Veto runs on the platform (`skip_veto: true`) | – | – | – |
| Wingman (`wingman: true` → `live_wingman.cfg`, 2v2) | `MatchConfig.cs` | none | missing | Exec a wingman cfg and set `game_mode`/`game_type` before `changelevel` | none | S | P2 |
| Workshop maps in `maplist` (digits → `host_workshop_map`) | `MapTargetLogic.cs` | done (RU: `plugins/match/readyup/map_names.cpp`): `123`, `ws:123`, `workshop/123[/name]` load with `host_workshop_map`; the loaded bsp name (or `workshop/<id>/<name>`) is bound to the id for `map_number`, MatchState and demo names | done | – | none | S | P1 |
| Simulation mode (`simulation`, `simulation_timescale`, bots play the match) | `SimulationMode.cs`, `SimulationRosterLogic.cs` | none (`dev_bots_ready` only for dev) | missing | Bot roster mapping + `host_timescale`. Useful for platform e2e tests. | none (mostly) | M | P2 |
| Stats DB (`ru_get_match_stats`, SQLite/MySQL) | `DatabaseStats.cs` | none | not needed | The platform stores stats from events | – | – | – |
| Sleep mode / safe auto-updater | `SleepMode.cs`, `AutoTournamentCS2SafeAutoUpdater.cs` | none | not needed | Out of plugin scope for Ready Up (CS2 update watch is in CI) | – | – | – |
| Skins | – | Ready Up extra (not in the default release) | extra | – | – | – | – |

## 8. Player stats (for `round_end` / `map_result`)

AT `PlayerStats` (`MatchData.cs:32-130`): `kills, deaths, assists, flash_assists, team_kills, suicides, damage, utility_damage, enemies_flashed, friendlies_flashed, knife_kills, headshot_kills, rounds_played, bomb_defuses, bomb_plants, 1k–5k, 1v1–1v5, first_kills_t/ct, first_deaths_t/ct, trade_kills, kast, score, mvp`.

Ready Up today (RU: `game_events.cpp` `EmitRoundEndLocked`): `kills, deaths, assists, headshot_kills, damage, mvps, score`. It uses `player_death`/`player_hurt`, and reads kills, deaths, assists, MVPs and score from `CCSPlayerController` schema fields.

Missing, and all computable from **standard game events** without new signatures:
- `player_death` (`assistedflash`, `weapon`, `attackerteam`, victim team): `flash_assists`, `team_kills`, `suicides`, `knife_kills`, `first_kills_*`/`first_deaths_*`, `trade_kills` (kill within 5 s of a teammate's death), multi-kills, clutches (alive counts per team).
- `player_hurt` with a grenade/molotov `weapon`: `utility_damage`.
- `player_blind`: `enemies_flashed`, `friendlies_flashed`.
- `bomb_planted` / `bomb_defused`: plants and defuses.
- `round_start`/`round_end`: `rounds_played`, `kast` (kill, assist, survived or traded, per round).

Effort **M**, engine **events** (already hooked). The stats must survive `mp_restartgame` and a restore: reset them at going-live, and snapshot them per round so a restore can roll back. Rename `mvps` → `mvp` in the payload (the normalizer accepts both).

---

## 9. Name mapping (platform `at_` → Ready Up)

Rule: `at_<x>` → `ru_<x>`. `css_*` and `get5_*` names stay as they are. "exists" = Ready Up already has an equivalent that the new name aliases.

| AT name | Ready Up name | Ready Up has now |
|---|---|---|
| `at_loadmatch_url` | `ru_loadmatch_url` | `ru match load` (Bearer) |
| `at_loadmatch` | `ru_loadmatch` | – |
| `at_clear_queued_match` | `ru_clear_queued_match` | – |
| `at_addplayer` / `at_removeplayer` | `ru_addplayer` / `ru_removeplayer` | – |
| `at_server_id` | `ru_server_id` | – |
| `at_bootstrap_url` / `at_bootstrap_token` | `ru_bootstrap_url` / `ru_bootstrap_token` | – |
| `at_clear_event_queue` | `ru_clear_event_queue` | – |
| `at_get_pending_events` | `ru_get_pending_events` | – |
| `at_remote_log_url` | `ru_remote_log_url` | `ru_webhook_url` (appends `/slug`) |
| `at_remote_log_header_key` / `_value` | `ru_remote_log_header_key` / `_value` | `ru_match_token` (Bearer) |
| `at_report_endpoint` / `at_report_token` / `at_report_server_id` | `ru_report_endpoint` / `ru_report_token` / `ru_report_server_id` | – |
| `at_match_report` | `ru_match_report` | `ru match state` (text) |
| `at_tournament_status` / `_match` / `_next_match` / `_updated` | `ru_tournament_status` / `_match` / `_next_match` / `_updated` | heartbeat `status` |
| `at_demo_upload_url` / `_header_key` / `_header_value` | `ru_demo_upload_url` / `_header_key` / `_header_value` | – |
| `at_demo_recording_enabled`, `at_demo_path`, `at_demo_name_format` | `ru_demo_recording_enabled`, `ru_demo_path`, `ru_demo_name_format` | always records |
| `at_loadbackup` / `at_loadbackup_url` / `at_listbackups` | `ru_loadbackup` / `ru_loadbackup_url` / `ru_listbackups` | boot recovery only |
| `at_remote_backup_url` / `_header_key` / `_header_value` | `ru_remote_backup_url` / … | – |
| `at_config_scope` | `ru_config_scope` | – |
| `at_chat_prefix`, `at_admin_chat_prefix` | `ru_chat_prefix`, `ru_admin_chat_prefix` | `readyup.cfg` keys |
| `at_knife_enabled_default`, `at_debug_chat` | `ru_knife_enabled_default`, `ru_debug_chat` | – |
| `at_minimum_ready_required`, `at_allow_force_ready`, `at_pause_after_restore`, `at_stop_command_available`, `at_stop_command_no_damage`, `at_whitelist_enabled_default`, `at_kick_when_no_match_loaded`, `at_playout_enabled_default`, `at_reset_cvars_on_series_end`, `at_use_pause_command_for_tactical_pause`, `at_autostart_mode`, `at_hostname_format`, `at_series_end_kick_delay_{no_demo,demo_no_upload,demo_upload}` | same with `ru_` | – |
| match `cvars{}`: `at_autoready_enabled`, `at_both_teams_unpause_required`, `at_max_pauses_per_team`, `at_pause_duration`, `at_side_selection_enabled`, `at_side_selection_time`, `at_gg_enabled`, `at_gg_threshold`, `at_gg_min_score_diff`, `at_ffw_enabled`, `at_ffw_time`, `at_demo_recording_enabled` | same with `ru_` | `knifeDecisionSeconds` (JSON field) |
| `at_event_retry_interval` | `ru_event_retry_interval` | fixed backoff |
| `.atversion` | `.ruversion` | `.ru version` |

Headers: keep **`X-Auto-Tournament-Token`** (sent as the configured `*_header_key`) and **`Auto-Tournament-FileName/MatchId/MapNumber/RoundNumber`** as they are. The platform owns these names. The report push uses `x-auto-tournament-token` (the same header, lowercase).

Match JSON: Ready Up already accepts the wrapper and the raw config, `matchid`, `num_maps`, `maplist`, `map_sides`, `team1/2.{name,players,captain_steamid64}`, `spectators`, `admins`, `cvars`, `maxRounds`, `overtimeMode`, `overtimeSegments` (RU: `match_config_parser.cpp`). It still needs `team1/2.id` (for `series_start`/`round_end` team objects), `team1/2.tag`, `team1/2.series_score`, `players_per_team`, `min_players_to_ready`, `min_spectators_to_ready`, `clinch_series`, `wingman`, `simulation`, `simulation_timescale`, `expected_players_*`, per-match `remote_log_*`, and `team1_t`/`team2_t` in `map_sides`.

---

## 10. Priorities: what blocks a real tournament

### P0: without these the platform cannot run a match on Ready Up

1. **Auth and URLs.** `ru_remote_log_url` used verbatim, plus `ru_remote_log_header_key/_value`. `ru_loadmatch_url <url> [hdr val]` with async fetch and AT reply strings. Today every event and config request is rejected (401) or goes to the wrong URL. (S+M)
2. **Status convars** `ru_tournament_status/_match/_next_match/_updated` with the AT enum (live = `playing`, like AT) and the `"name" = "value"` RCON reply. Allocation depends on them. (S)
3. **Bootstrap flow**: `ru_server_id`, `ru_bootstrap_token`, `ru_bootstrap_url` (debounced async GET, run and persist `commands[]`), `ru_clear_event_queue`, `ru_report_*`, and accept-and-persist every §2 setting as a known command, so none of them reaches the engine as "Unknown command". (M)
4. **Event payload shapes**: `winner:{side,team}`, `team1/team2` objects with `score` and `players[].stats`, `series_start` team objects, and `map_result` with final player stats. `server_configured` with the real `server_id`, plus `test_event` (`css_te`) for the connection test. (M)
5. **Lifecycle aliases the allocator calls**: `css_endmatch` and `css_restart` with AT semantics (end/reset, clear the queue), `css_start`, and `css_pause`/`css_unpause`/`css_forcepause`/`css_forceunpause`. Also match load while in postgame: queue it (`queued_match=`) or at least give an unambiguous reply. Today Ready Up silently replaces the running series. (M)
6. **Ready threshold**: `ru_minimum_ready_required` / `min_players_to_ready`. Today a roster with a substitute never goes live. (S)
7. **Demo upload + demo events** (`demo_recording_*`, `demo_upload_*`), with the `Auto-Tournament-*` headers, and the series-end kick delays. Server turnover after a series depends on them, and demos are an expected tournament deliverable. (M)
8. **Platform one-liner**: the plugin version check must compare `server_configured.plugin_version` against `Auto-Tournament/ready-up` releases instead of `cs2-plugin`. (platform, S)

### P1: needed for a tournament run without an admin babysitting it

1. Round restore: `css_restore <n>`, `ru_loadbackup`/`_url`, `ru_listbackups`, the `backup_loaded` event. (M)
2. Pauses: tactical vs technical, `.tac` timeouts through the built-in `timeout_*_start`, per-team limits and duration (`ru_max_pauses_per_team`, `ru_pause_duration`), `ru_both_teams_unpause_required`, correct `is_tactical`/`pause_time`. (M)
3. Full player stat set (§8), including KAST, ADR inputs and utility damage, plus a stats snapshot and rollback on restore. (M)
4. Match report push/pull (`ru_report_endpoint`, `ru_match_report`) for the live match page. (M)
5. `ru_addplayer`/`ru_removeplayer` (roster substitutions from the web UI). (S)
6. In-game team names (`mp_teamname_1/2`), and `css_switch`/`css_asay`/`css_map`/`reload_admins`/`css_prac` aliases for the web admin buttons. (S)
7. ~~Workshop maps (`host_workshop_map`).~~ Done. (S)
8. Forfeit when a team leaves (`ru_ffw_*`), `.forceready`, and side selection time from `ru_side_selection_time`. (S each)
9. Persist the series score and the event retry queue across restarts. Move all HTTP off the game thread. (S+M)

### P2: parity and polish

Coach (L, fragile if emulated), `.stop` vote, `.gg` vote, `ru_autoready_enabled`, damage report, wingman, simulation mode, the remaining admin chat commands (`.settings`, `.team1`, `.readyrequired`, `.playout`, `.roundknife`, `.whitelist`, `.rcon`), `ru_hostname_format`, `ru_kick_when_no_match_loaded`, `ru_autostart_mode`, `cs2_update_required`, `server_health` with real DB state, `ru_config_scope`, practice extras (nades/spawns: L, fragile), remote backup upload.

### Rough total

P0 ≈ 6–8 days. P1 ≈ 6–8 days. None of P0 or P1 needs new signatures or offsets: all of it uses HTTP, built-in console commands and cvars, and the game events Ready Up already hooks. The only fragile items are coach emulation and the practice nade/spawn tools, both P2.
