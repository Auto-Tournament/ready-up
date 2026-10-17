# tv plugin ("broadcast"): GOTV settings that hold for a whole series

Status: implemented in `plugins/tv` (tv.so), not released. Owner of the GOTV (CSTV) side of a
server: the delay, the client limit, chat / voice relay, and whether observers with an insecure
client (HLAE, used by HUD tools such as Lexogrine HUD Manager) may watch over GOTV.

## Why

At a LAN final a streamer ran Lexogrine HUD Manager (LHM). LHM's HLAE features need CS2 started
with `-insecure`, and a VAC-secured server refuses insecure clients, on its GOTV port too. The fix
LHM documents (guide.lhm.gg, Counter-Strike page) is two server cvars:

- `tv_secure_bypass 1`: insecure clients may connect to GOTV (only GOTV; the game port stays
  VAC-secured).
- `sv_deltaticks_enforce 0`: stops the "tick ack out of order" / "false tick interval" kicks those
  observers get.

They were typed into the console by hand. They can reset on a map change or a cfg exec, and
re-applying them from outside the server (the platform or csm sending `exec` lines on a timer)
was rejected: it races the match flow and needs a person or a cron per server. The server has to
own its broadcast settings and put them back itself.

## Goals

1. A **broadcast profile**: `delay_seconds`, `allow_insecure_observers`, `max_clients`,
   `relay_text_chat`, `relay_voice`. Server defaults (`cfg/ReadyUp/tv.cfg`, changed at runtime
   with `ru tv set` / fleet `tv.set`), plus a per-match override from `match.assign`
   `config.rules.broadcast`.
2. **Applied and kept**: on load, on assign, after every map load, `mp_restartgame` (match start)
   and match phase change (the match plugin execs cfgs then), and when another command sets one
   of the owned cvars to something else. Restored to the server defaults on unassign; a cvar
   neither the server nor the match sets goes back to the value it had before tv.so first changed
   it.
3. `allow_insecure_observers` is one switch: `tv_secure_bypass` and `sv_deltaticks_enforce` move
   together, never one without the other.
4. Admin commands: `ru tv status|set|match|caster|reapply` on the console / RCON, `.ru tv ...` and
   `.tv ...` in chat for admins.
5. Fleet: GOTV state in `hello.tv`, `state.snapshot.tv` and a reliable `server.tv` on change;
   platform command `cmd tv.set`. Schemas proposed in `plugins/fleet/protocol/v1`.
6. Casters: a server-level list of SteamID64s who may join the **game port** as spectators
   without being kicked, through the whitelist plugin's API (`readyup.whitelist.v1`
   `set_guests`), honoured by the whitelist plugin and by the match plugin's own roster check.
7. Relay servers documented (below), not run.

## Non-goals

- Running GOTV relays (process management, ports, csm). Documented only.
- `tv_broadcast` / CSTV+ HTTP broadcast: the match ruleset already has `tv_broadcast_url`.
- The second GOTV instance (`tv_enable1`, `tv_delay1`). Profile applies to instance 0.
- Managing `tv_password`. The platform sets it per match in `config.cvars` (the match plugin
  applies it). tv.so only reports whether one is set.
- Anything for players: no client checks, no change to VAC for the game port.
- Standalone (non-fleet) per-match profiles: a match loaded from a file (`ru match load`) gets
  the server profile only.

## Settings

| Key (aliases) | Type | Range | cvars | Unset means |
|---|---|---|---|---|
| `delay_seconds` (`delay`) | int | 0..960 | `tv_delay` | tv.so leaves `tv_delay` alone (match ruleset / cfg own it) |
| `allow_insecure_observers` (`insecure`) | bool | | on: `tv_secure_bypass 1` + `sv_deltaticks_enforce 0`; off: `tv_secure_bypass 0` (+ `sv_deltaticks_enforce` back to its baseline) | left alone |
| `max_clients` (`maxclients`) | int | 0..255 | `tv_maxclients` | left alone |
| `relay_text_chat` (`textchat`) | bool | | `tv_relaytextchat` | left alone |
| `relay_voice` (`voice`) | bool | | `tv_relayvoice` | left alone |

Precedence, per key: built-in "unset" < `tv.cfg` < runtime server setting (`ru tv set`, fleet
`tv.set {scope: server}`, saved in `csgo/readyup/plugins/tv/settings.json`) < the match profile
(`rules.broadcast`) < a runtime match override (`ru tv match`, `tv.set {scope: match}`, dropped
with the match).

**Delay floor.** While a match is loaded tv.so never sets `tv_delay` below what the match itself
asks for: `config.cvars.tv_delay`, `rules.overrides.tv_delay`, or 105 under `rules.ruleset:
"valve"` (Valve's Premier value, `plugins/match/readyup/ruleset.cpp`). A lower GOTV delay lets a
viewer ghost for a team, so this is integrity, not taste. `match.assign` is refused
(`cmd.result invalid_config`) when `rules.broadcast.delay_seconds` is below the floor or when
`rules.broadcast` and `config.cvars` disagree on a cvar; the match plugin and tv.so share the
validator (`libs/readyup/broadcast_profile.h`). A server default below the floor is raised to
the floor for that match (and `ru tv status` says so).

Other `tv.cfg` keys: `casters` (comma separated SteamID64s), `clients_poll_seconds` (default 60;
how often `tv_status` is read for the client count while the fleet link is up; 0 = only on
`ru tv status`), `enabled` (default 1; 0 = load but touch nothing).

## How applying works

- **Baseline.** Before tv.so first changes a cvar it reads its current value (`cvar_query`, ru_api
  1.11). When nobody sets that key any more (unassign, `ru tv set <key> default`) it writes the
  baseline back. Baselines and what is applied survive `ru plugin reload tv` (core stash). If a
  baseline could not be read, `tv_secure_bypass` goes back to `0` (the safe value) and the other
  cvars are left as they are (logged).
- **Re-apply triggers.** Map start (now, +2 s, +8 s: after `server.cfg`, the game mode cfgs and
  the match plugin's `EnqueueAfterCfg`), `Match_Start`, a change of the match plugin's ru mode
  (+1 s, +5 s), assign / unassign / any setting change (now), `ru tv reapply`, and an observed
  console line that sets an owned cvar to another value (+1 s). A re-apply sends every desired
  value again (cheap, idempotent); there is no cvar polling.
- **Verify.** 3 s after a re-apply tv.so reads the owned cvars back and reports what the engine
  has (`observed` in the state); a mismatch is logged once per value.
- **Unassign.** Detected from `match.unassign`, or from the match plugin no longer reporting the
  match id (series end, `ru match end`, a rejected assign). The match profile is dropped and the
  server profile applied.

## Commands

Console / RCON, and `.ru tv ...` or `.tv ...` in chat (admins only; `is_admin`):

```
ru tv [status]                       GOTV port, enabled, delay (desired / engine), clients x/max,
                                     secure bypass + deltaticks, relay chat / voice, password set,
                                     profile source (server / match <id>), casters
ru tv set <key> <value|default>      server setting, saved; applies now unless the match overrides it
ru tv match <key> <value|default>    this match only (refused with no match loaded)
ru tv caster add|remove <steamid64>  server caster list (saved)
ru tv caster list|clear
ru tv reapply                        send every desired value again now
ru tv help
```

Values: integers, `1/0/on/off/true/false/yes/no`, `default` (or `-`) clears the key.

## Casters on the game port

A caster sits in the server (game port), not on GOTV, to talk to the teams or run an observer
camera. The whitelist plugin (outside a match) and the match plugin (during a match, roster +
`spectators`) kick anyone else. Instead of a second allow-list, `readyup.whitelist.v1` gets two
appended members:

- `set_guests(owner, steamids, count)`: a plugin's guest list, replaced as a whole, not saved.
- `is_guest(steamid64)`.

tv.so pushes its caster list as owner `"tv"` (again every 5 s, so a whitelist reload is healed,
and `count 0` on unload). The whitelist plugin never kicks a guest; the match plugin's
`EnforceWhitelist` lets guests in and its team lock keeps them on spectators (like the match's
own `spectators`). Without whitelist.so the match plugin does not see guests: per-match casters
still go in `config.spectators`, which is the right place for a match-specific caster anyway.

## Fleet protocol additions (proposed; the platform must mirror them)

All in `plugins/fleet/protocol/v1`, written in the platform's style:

| File | What |
|---|---|
| `tv.defs.json` (new) | `$defs/broadcastProfile` (the five keys, all optional, `additionalProperties: false`) and `$defs/tvState` |
| `match.defs.json` | `rules.broadcast` → `tv.defs.json#/$defs/broadcastProfile` |
| `messages/server.tv.json` (new) | server → platform, reliable: `tvState`, sent when it changes (at most every 5 s) |
| `messages/hello.json` | optional `tv`: `tvState` |
| `messages/state.snapshot.json` | optional `tv`: `tvState` |
| `messages/cmd.json` | `name` enum + `tv.set`; args documented |
| examples | `cmd.tv.set.json`, `server.tv.json`, `hello.tv.json`, `match.assign.broadcast.json` |

`tvState`:

```json
{
  "available": true,             // tv.so loaded and applying (false: enabled=0 in tv.cfg)
  "port": 27020,                 // GOTV port (+tv_port, else tv_port cvar); null = unknown
  "enabled": true,               // tv_enable as the engine reports it; null = unknown
  "delay_seconds": 105,          // engine value (observed); null = unknown
  "max_clients": 8,
  "clients": 3,                  // GOTV spectators from tv_status; null = not read yet
  "secure_bypass": true,         // tv_secure_bypass
  "deltaticks_enforce": false,   // sv_deltaticks_enforce
  "relay_text_chat": true,
  "relay_voice": true,
  "password_set": true,          // a non-empty tv_password is set; null = unknown. NEVER the value
  "profile": {"source": "match", "match_id": "m_123", "settings": {"delay_seconds": 105, "allow_insecure_observers": true}},
  "delay_floor": 105,            // the match's own minimum; 0 = none
  "casters": 2,                  // count only
  "server_insecure": false       // the server process itself runs -insecure (not VAC-secured)
}
```

`cmd tv.set` args: `{scope: "server" | "match", settings?: {<key>: value | null}, casters?:
[SteamID64 string] (server scope only, replaces the list)}`; at least one of `settings` /
`casters`. All or nothing. Errors: `bad_args` (unknown key, type, range, below the delay floor),
`bad_phase` (`scope: match` with no match loaded), `unsupported` (tv.so not loaded), `io` (could
not save). fleet.so owns the command (like `whitelist.set`) and hands the args to tv.so's
`readyup.tv.v1`.

Capability: tv.so adds `tv.v1` to hello's capabilities.

The platform repo's fleet-protocol-sync test compares these files with its copy, so the platform
must copy, unchanged: `tv.defs.json`, `messages/server.tv.json`, and the edited `match.defs.json`,
`messages/{hello,state.snapshot,cmd}.json`, plus the four example frames.

What the platform then needs to build: a "Broadcast" block in the match / tournament settings
(the five keys, written to `rules.broadcast`), a server page row for `tv` (port, delay, clients,
bypass, password set), the `tv.set` admin action (with audit row), storing `server.tv`, and the
caster list in the server settings.

## Security notes

- **Insecure observers only on GOTV.** `tv_secure_bypass` lets insecure clients connect to the
  GOTV port. The game port still refuses them; VAC for players is unchanged. Nothing in tv.so
  touches `-insecure` or `sv_lan`. A server started `-insecure` itself is not VAC-secured at all;
  `ru tv status` and `tvState.server_insecure` say so, so nobody mistakes the bypass for the cause.
- **`sv_deltaticks_enforce 0` is server-wide.** It relaxes a tick-ack sanity check for every
  client, players included, not only GOTV. Valve does not document what it guards against. That is
  why it is never set on its own: it is on only while `allow_insecure_observers` is on, and goes
  back with it. Leave the switch off unless an HLAE / LHM observer needs it.
- **GOTV delay.** Never lowered below the match's own floor (see above).
- **GOTV password.** tv.so never reads `tv_password` with `cvar_query` (the engine would print it
  to the console log) and never logs or sends it. `password_set` comes from the assign's
  `config.cvars.tv_password`, an observed `tv_password` console line (only "empty or not" is kept)
  or `+tv_password` on the command line. The match plugin used to print match cvars with their
  values at `debug=1` and read every match cvar back for its restore snapshot (the engine echoes
  the old value): both now skip `*_password` cvars (`plugins/match/readyup/cvar_snapshot_glue.cpp`,
  `modes.cpp`).
- **Platform leak (not fixed here).** The platform's public match API returns the GOTV password.
  It must only go to allowed viewers (the `/api/events/watch` path) and to the server in
  `match.assign`; every other response must drop it or send `password_set: true` instead.
- **Casters** get no rights: they are only not kicked, and kept on spectators during a match.

## Relay servers (documented, not implemented)

A GOTV relay is a second CS2 process that connects to the match server's GOTV and re-serves it,
so a stage of viewers does not load the game server.

- Master (the match server): `tv_enable 1`, `tv_maxclients` small (the relay counts as one
  client), `tv_relaypassword <p>` (relays log in with it, spectators use `tv_password`),
  `tv_allow_static_shots`/`tv_dispatchmode` as today.
- Relay: started with `+tv_relay <master-ip>:<tv_port>` and `+password <tv_relaypassword>`, its own
  `-tv_port`, `tv_maxclients` large, `tv_secure_bypass 1` too if insecure observers watch the relay
  (the bypass is per process). No map, no players: it follows the master.
- What csm would need: a "relay" server kind (no match, no fleet enroll, `+tv_relay` args from the
  master's public address and GOTV port), the relay password from the platform (per match or per
  event; never in a public API), start/stop tied to a match or an event stage, and the relay's
  port in the platform's watch links instead of the master's. tv.so could report
  `relay_password_set` and the master's `tv_relay*` cvars once that exists.

## Open questions

- `tv_status` output on current CS2: the parser accepts the CS:GO shape (`Local Slots N,
  Spectators N, Proxies N`) and a `GOTV[0]:`-prefixed variant; check it on a test server and add
  the real line to the tests.
- Is `tv_secure_bypass` honoured when the bypass is set after GOTV started on the map, or only at
  map load? The re-apply at map start covers both; confirm on a test server.
- Default of `sv_deltaticks_enforce` on current CS2 (the baseline read handles either).
- Should `rules.broadcast` also carry `tv_delay1` / the second instance for broadcast tools that
  use it?
- Per-match profiles for matches loaded from a file (no fleet): add `broadcast` to the get5 /
  MAT config shape?
- `match.update` cannot change `rules.broadcast` today (only `tv.set {scope: match}`); does the
  platform need it in `update` ops?
