# Admins

Ready Up has no database (docs/FLEET.md D13). Who counts as an admin:

- the per-match `admins` of the loaded match config,
- the MAT admin list (`ru_admins_url`), if configured,
- `admins.json` of the essentials plugin (`plugins/essentials/admins.json`), always,
- **fleet mode** (`[fleet] url` set): also the platform's fleet-wide list.

## `admins.json`

`game/csgo/readyup/plugins/essentials/admins.json` (the essentials plugin; on its first load it copies an
older `plugins/match/admins.json` there)

```json
{
  "version": 1,
  "admins": [
    { "steamid64": "76561198000000000", "name": "alice" }
  ]
}
```

`ru admins add|remove` write it for you. You can also edit it by hand: Ready Up re-reads it
within 30 s when it changes. Write it atomically (to a temp file, then rename). A file that is
not valid JSON or has no `"version"` is moved to `admins.json.corrupt-<time>`, logged, and Ready
Up starts with no admins from the file. The server console can always add admins again.

Each server has its own file. To share admins between servers, copy the file, point them at the
same MAT admin list (`ru_admins_url`), or use fleet mode.

## Fleet mode: the platform's list

In fleet mode the platform sends the whole admin list (`admins.set`, D5). Ready Up caches it in
`game/csgo/readyup/plugins/match/fleet-admins.json`, so a server that starts while the platform
is unreachable still knows its admins. `admins.json` still counts on top of it: a local admin
(the server owner) stays an admin even when the platform sends no list or a list without them.
`ru admins list|add|remove` work on `admins.json`; the platform's list is edited on the platform.

## Commands

`ru` commands are main commands with subcommands: `.ru <command> <subcommand> [args]` in chat,
`ru <command> <subcommand> [args]` on the server console / RCON, or `ru ...` in a player's own
game console (like Metamod's `meta` / CounterStrikeSharp's `css_` commands: same admin checks,
nothing shows in chat). `.ru help` lists the main commands, one chat line each; `.ru help
<command>` (or just `.ru <command>`) lists its subcommands. An unknown command answers "unknown
command, type .ru help". Help, state and errors go to the sender only; actions that concern
everyone (a pause, a map change) are announced to all.

Admin-only commands answer "not authorized" to anyone else and do nothing; the server console
always may. An admin's `.help` points at `.ru help`.

| Command | Who | Does |
|---|---|---|
| `.ru match load <url>` | admin | loads a match config (http/https; the URL is visible in chat) |
| `.ru match start [force]` | admin | force-starts the loaded match (`force`: also under the valve ruleset without GOTV, [ESPORTS-MODE.md](ESPORTS-MODE.md)) |
| `.ru match restart` | admin | the loaded match back to its warmup; everyone readies again |
| `.ru match end` | admin | ends the loaded match (`series_end` winner none) and resets the server |
| `.ru match recover [round]` | admin | asks the platform to recover the match (`recover_requested`) |
| `.ru match pause` / `unpause` | admin | admin pause / unpause (`.fp` / `.fup` in chat) |
| `.ru match tech\|tac team1\|team2` | admin | technical pause / tactical timeout for a team, with its limits |
| `.ru match side stay\|switch\|ct\|t` | knife winners, admin | knife side pick (`.stay` / `.switch`) |
| `.ru match state` / `rules` | everyone | match and mode state / effective rules |
| `.admin [message]` | everyone | calls an admin ([below](#calling-an-admin-admin)); not an `ru` command |
| `.ru map change <name\|workshop id\|link> [force]` | admin | `changelevel <name>`, or `host_workshop_map <id>` for `3084291314`, `ws:<id>`, `workshop/<id>[/name]` or a pasted Workshop link (`…/filedetails/?id=3084291314`); refused during a knife round or a live map unless `force` (essentials plugin) |
| `.ru map reload [force]` | admin | loads the current map again (a workshop map by its id) (essentials plugin) |
| `.ru map restart [force]` | admin | restarts the game (`mp_restartgame 1`); a loaded match stays loaded (essentials plugin) |
| `.ru map defaults` / `.ru map default <mode> [<map>\|clear]` | everyone / admin to set | the default map per mode (`ffa`, `tdm`, `practice`, `warmup`, `retakes`, ...), `plugins/essentials/default_maps.json` (essentials plugin) |
| `.ru mode show` | everyone | the current mode |
| `.ru mode idle` / `practice` / `scrim` | admin | plain CS2 / practice mode (toggles, `.prac`; needs the practice plugin) / auto scrim warmup back on |
| `.ru practice on\|off\|status` | admin | practice plugin: practice mode (`status`: everyone) |
| `.ru dm ffa\|tdm [map]` / `.ru dm off` | admin | deathmatch plugin: free for all / team deathmatch (CS2's deathmatch game mode; loads the map given, else the mode's default map) / back to competitive; `.ru dm status\|top\|hud`: everyone ([DEATHMATCH.md](DEATHMATCH.md)) |
| `.ru admins list` | everyone | the admins (essentials plugin) |
| `.ru admins add\|remove <steamid64\|name_fragment>` | admin (standalone) | edit `admins.json` |
| `.ru hud test <1-11>` | admin | a HUD test panel, to you only |
| `.ru whitelist on\|off\|add\|remove\|list\|clear` | admin | whitelist plugin: only listed players may stay (not during a match) |
| `.ru plugin list\|load\|unload\|reload <name>` | admin | plugins (core); load / unload last until a restart |
| `.ru plugin enable\|disable <name>` | admin | load / unload a plugin and keep it that way after a restart (`csgo/readyup/plugins/plugins.json`) |
| `.ru reload` | admin | reloads `readyup.cfg` (core) |
| `.ru selftest` / `.ru version` | admin / everyone | core |

Before this layout the match commands were flat (`ru start`, `ru end`, `ru idle`, `ru state`,
`ru side`, `ru fp`, ...). Those names are gone: use the table above. `ru match load <url>` is
unchanged.

Notes:
- If no admins exist yet, the **first admin must be added from the server console** (or in
  `admins.json`).
- SteamID input is **SteamID64** (decimal) or a connected-player name fragment.

## Calling an admin (`.admin`)

Any player (roster, spectator, scrim, any mode) can type `.admin [message]` in chat:

- The caller gets a private "admins notified.". Each player can call once every
  `admin_call_cooldown_s` seconds (`readyup.cfg`, default 60, 0 = no cooldown); a call during the
  cooldown only answers "you can call again in Ns".
- Every admin in game gets a private chat line (`ADMIN CALL <name> (<team>, <side>) needs an
  admin: <message>`) and a center card for about 6 s ("ADMIN CALLED", same text) at
  `RU_HTML_PRIO_ALERT` ([HUD.md](HUD.md)).
- The server console logs `admin-call: <name> (<steamid64>, <team>): <message> [<call_id>]`.
- The platform gets an `admin_called` event: through the webhook pipeline (same events URL,
  token and retry queue as `match_paused` & co; the token goes in both `Authorization: Bearer`
  and `X-Auto-Tournament-Token`), and on the fleet link as
  `event.admin_called` (while the server has a platform assignment; `data` = the same fields
  without `event` / `matchid` / `map_number`, schema
  `plugins/fleet/protocol/v1/messages/event.admin_called.json`).

Resolving a call is done on the platform; there is no in-game command for it.

The webhook body (POSTed to `<events url>/<match slug | matchid>`; with no match loaded
`<events url>/unknown`, in a scrim `<events url>/scrim`):

```json
{
  "event": "admin_called",
  "matchid": 4242,
  "map_number": 0,
  "server_id": "srv-eu-1",
  "call_id": "3f2b8c1e-9a4d-4e6f-8b21-7c5d0e9f1a2b",
  "player": { "steamid64": "76561198000000001", "name": "alice", "team": "team1", "side": "ct" },
  "message": "smoke bugged on B site",
  "called_at": "2026-09-25T12:40:12.345Z"
}
```

- `matchid`: the loaded match's id (a number); `-1` in a scrim or with no match loaded.
- `map_number`: the current map of the series, **0-based** (0 = the first map; 0 without a match).
  The other webhook events count maps from 1.
- `server_id`: the fleet server id, only when the server is enrolled in fleet mode.
- `call_id`: unique per call (a version-4 UUID).
- `player.steamid64`: a string. `player.team`: `"team1"` / `"team2"` (the match roster),
  `"spectator"` (on the spectator team) or `null` (scrim player, not on the roster).
  `player.side`: `"ct"` / `"t"` / `null`.
- `message`: what followed `.admin`, trimmed, chat color bytes removed, at most 200 characters;
  may be empty.
- `called_at`: ISO 8601 UTC with milliseconds.

## Upgrading from Postgres

Older versions kept admins in Postgres (`readyup_db.json`). Run the migration once; it copies
admins, persisted settings and skins loadouts into the JSON files and only reads the database:

```bash
python3 game/csgo/readyup/tools/migrate-postgres-to-json.py --csgo game/csgo
# Postgres in a container without psql on the host:
python3 game/csgo/readyup/tools/migrate-postgres-to-json.py --csgo game/csgo --docker readyup-postgres
```

See [INSTALL.md](INSTALL.md#upgrading-from-postgres).
