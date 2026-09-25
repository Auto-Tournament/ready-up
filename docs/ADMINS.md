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

- **Public**:
  - `.ru admins` or `.ru admins list`
- **Admin-only** (standalone):
  - `.ru admins add <steamid64|name_fragment>`
  - `.ru admins remove <steamid64|name_fragment>`

## In-game server controls

Admin-only in chat (`.ru <command>`); the server console / RCON (`ru <command>`) always may. A
non-admin gets "not authorized" and nothing runs. An admin's `.help` lists them too.

| Command | Does |
|---|---|
| `.ru map <name\|workshop id>` | `changelevel <name>`, or `host_workshop_map <id>` for a workshop id (`3084291314`, `ws:<id>`, `workshop/<id>[/name]`) |
| `.ru reloadmap` | loads the current map again (a workshop map by its id) |
| `.ru restart` | restarts the game (`mp_restartgame 1`); a loaded match stays loaded |
| `.ru load <url>` | loads a match config (same as `ru match load <url>`); the URL is visible in chat |
| `.ru end` | ends the loaded match (`series_end` winner none) and resets the server |
| `.ru match restart` | the loaded match back to its warmup; everyone readies again (this was `ru restart` before) |
| `.ru start` | force-starts the loaded match |
| `.ru reload` | reloads `readyup.cfg` (core) |

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
