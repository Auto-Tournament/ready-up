# Admins

Ready Up has no database (docs/FLEET.md D13). Who counts as an admin:

- the per-match `admins` of the loaded match config,
- the MAT admin list (`ru_admins_url`), if configured,
- **standalone**: `admins.json` in the match plugin's data dir,
- **fleet mode** (`[fleet] url` set): the platform's fleet-wide list instead of `admins.json`.

## Standalone: `admins.json`

`game/csgo/readyup/plugins/match/admins.json`

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
is unreachable still knows its admins. `admins.json` is ignored. `ru admins list` shows the
platform's list and its rev. `add` and `remove` answer "Admins are managed on the platform".

## Commands

`ru` commands are main commands with subcommands: `.ru <command> <subcommand> [args]` in chat,
`ru <command> <subcommand> [args]` on the server console / RCON. `.ru help` lists the main
commands, `.ru help <command>` (or just `.ru <command>`) lists its subcommands. An unknown command
answers "unknown command, type .ru help". Help goes to the sender only.

Admin-only commands answer "not authorized" to anyone else and do nothing; the server console
always may. An admin's `.help` points at `.ru help`.

| Command | Who | Does |
|---|---|---|
| `.ru match load <url>` | admin | loads a match config (http/https; the URL is visible in chat) |
| `.ru match start` | admin | force-starts the loaded match |
| `.ru match restart` | admin | the loaded match back to its warmup; everyone readies again |
| `.ru match end` | admin | ends the loaded match (`series_end` winner none) and resets the server |
| `.ru match recover [round]` | admin | asks the platform to recover the match (`recover_requested`) |
| `.ru match pause` / `unpause` | admin | admin pause / unpause (`.fp` / `.fup` in chat) |
| `.ru match tech\|tac team1\|team2` | admin | technical pause / tactical timeout for a team, with its limits |
| `.ru match side stay\|switch\|ct\|t` | knife winners, admin | knife side pick (`.stay` / `.switch`) |
| `.ru match state` / `rules` | everyone | match and mode state / effective rules |
| `.ru map change <name\|workshop id>` | admin | `changelevel <name>`, or `host_workshop_map <id>` for `3084291314`, `ws:<id>`, `workshop/<id>[/name]` |
| `.ru map reload` | admin | loads the current map again (a workshop map by its id) |
| `.ru map restart` | admin | restarts the game (`mp_restartgame 1`); a loaded match stays loaded |
| `.ru mode show` | everyone | the current mode |
| `.ru mode idle` / `practice` / `scrim` | admin | plain CS2 / practice mode (toggles, `.prac`) / auto scrim warmup back on |
| `.ru admins list` | everyone | the admins |
| `.ru admins add\|remove <steamid64\|name_fragment>` | admin (standalone) | edit `admins.json` |
| `.ru hud test <1-7>` | admin | a HUD test panel, to you only |
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

## Upgrading from Postgres

Older versions kept admins in Postgres (`readyup_db.json`). Run the migration once; it copies
admins, persisted settings and skins loadouts into the JSON files and only reads the database:

```bash
python3 game/csgo/readyup/tools/migrate-postgres-to-json.py --csgo game/csgo
# Postgres in a container without psql on the host:
python3 game/csgo/readyup/tools/migrate-postgres-to-json.py --csgo game/csgo --docker readyup-postgres
```

See [INSTALL.md](INSTALL.md#upgrading-from-postgres).
