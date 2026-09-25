# Install

## Supported setup

Ready Up runs on Linux dedicated servers (`linuxsteamrt64`). Release builds need nothing on
the host beyond glibc 2.31+ (OpenSSL and libcurl are linked in). There is no database: admins,
persisted settings, match recovery state and skins loadouts are small JSON files in
`game/csgo/readyup/plugins/<plugin>/` (see [Data files](#data-files)).

Metamod can run alongside Ready Up. In `gameinfo.gi`, Metamod's `Game csgo/addons/metamod`
line stays first, Ready Up's `Game csgo/readyup` goes directly below it, and both stay above
`Game csgo`. With Ready Up above Metamod, Metamod never loads (and neither does anything it
loads, such as CounterStrikeSharp); Ready Up logs a `load-order: WARNING` line at startup.
We recommend running Ready Up on its own, and never next to another match plugin: see
[COMPATIBILITY.md](COMPATIBILITY.md) for what was tested and what breaks.

## Install with the installer

From the server root (the folder with `game/`), as the server's user:

```bash
curl -fsSL https://raw.githubusercontent.com/Auto-Tournament/ready-up/master/install.sh | bash
```

See the [README](../README.md#install) for the checklist, the non-interactive forms
(`essentials`, `full`, `--yes`, `--remove`, `--uninstall [--purge]`, `--zip`, `--version`,
`--dir`) and what it touches. It records what is installed in
`game/csgo/readyup/installed.json` (component -> version) and each component's file list in
`game/csgo/readyup/manifests/<component>.json`, which is how updates remove files a newer
version no longer ships and how unticking a component removes it.

## Install from a release zip by hand

Every zip's root is the contents of `game/csgo`:

| Zip | Contents |
|---|---|
| `ready-up-essentials-<v>-linuxsteamrt64.zip` | core + match + fleet (default, no skins) |
| `ready-up-full-<v>-linuxsteamrt64.zip` | core + match + fleet + skins + hello + `readyup_sigcheck` / `readyup_hookcheck` |
| `ready-up-core-...`, `-match-...`, `-fleet-...`, `-skins-...`, `-hello-...` | single components. The core runs alone, but the match flow (ready-up, knife, pauses, webhooks) is `match`. `fleet` links the server to the Auto Tournament platform and stays idle until configured ([FLEET.md](FLEET.md)). |
| `SHA256SUMS` | checksums of every zip |

1. Extract the zip into `game/csgo`. You should end up with `game/csgo/readyup/bin/linuxsteamrt64/libserver.so`.
2. Add Ready Up to `gameinfo.gi`:

   ```bash
   cd game/csgo
   python3 readyup/tools/patch_gameinfo.py gameinfo.gi --game csgo/readyup
   ```

   If you have `gameinfo_branchspecific.gi`, run it on that file too. The patcher is
   idempotent, fixes a misplaced or duplicated entry, and writes a
   `.readyup-backup-<timestamp>` copy whenever it changes the file. `--check` only reports
   (exit 3 = needs patching); `--remove` takes the line out again.
3. Copy `readyup/bin/linuxsteamrt64/readyup.cfg.example` to `readyup.cfg` next to it and
   `readyup/cfg-templates/ReadyUp/*.cfg` to `game/csgo/cfg/ReadyUp/` if you don't have them yet.
4. Restart the server.

CS2 updates rewrite `gameinfo.gi`, so run the patcher (or the installer) again after each one.

The zips only contain files Ready Up owns, so extracting a newer one over an install keeps
your `readyup.cfg`, `cfg/` edits and the plugins' JSON data:

- `readyup/bin/linuxsteamrt64/libserver.so` (the core)
- `readyup/bin/linuxsteamrt64/engine-surface.json` (signatures + identity anchors, also embedded in the core)
- `readyup/bin/linuxsteamrt64/readyup.cfg.example`
- `readyup/tools/patch_gameinfo.py`, `readyup/tools/install.sh`,
  `readyup/tools/migrate-postgres-to-json.py`
- `readyup/cfg-templates/ReadyUp/*.cfg` (mode cfgs, only used when cfg exec is enabled)
- `readyup/VERSION`, `README.md`, `INSTALL.md`, `LICENSE`, `BUILD_INFO` (commit + the CS2 build it was verified against)
- skins only: `readyup/plugins/skins.so`, `readyup/bin/linuxsteamrt64/engine-surface.skins.json`, `readyup/SKINS-WARNING.txt`
- fleet only: `readyup/plugins/fleet.so`, `readyup/cfg-templates/ReadyUp/fleet.cfg` (copied to
  `cfg/ReadyUp/fleet.cfg` if missing; every line is commented out, so the link stays idle until
  you set `url` and an enrollment code or key). Its data dir `readyup/plugins/fleet/`
  (`install_id`, `credentials.json`, spool) is never shipped and survives updates and removal.
- hello only: `readyup/plugins/hello.so`
- `readyup/manifests/<component>.json`

## Data files

Ready Up keeps its state in small JSON files, one directory per plugin. Every file has a
`"version"`, is replaced atomically (temp file + rename) and is never shipped in a zip, so
updates keep it. A file that is broken or from a newer version is moved to
`<name>.corrupt-<time>`, logged, and the plugin starts fresh.

| File (under `game/csgo/readyup/plugins/`) | What | Written by |
|---|---|---|
| `match/state.json` | persisted settings (`ru_webhook_url`, `ru_heartbeat_url`, `ru_match_token`, `ru_admins_url`, ...) and the crash-recovery match state | the match plugin |
| `match/admins.json` | standalone admins ([ADMINS.md](ADMINS.md)) | `ru admins add/remove`, or by hand |
| `match/fleet-admins.json` | fleet mode: cached platform admin list | the match plugin |
| `skins/loadouts.json` | standalone skins loadouts ([json-contract.md](../plugins/skins/docs/json-contract.md)) | you / a web tool / `scripts/seed-dev-skins.py` |
| `skins/stattrak.json` | standalone StatTrak counters | the skins plugin |

Back up the directory to keep admins and loadouts; delete `match/state.json` to forget persisted
settings and a half-finished match.

### Console settings that survive a restart

These match settings are saved in `match/state.json` (`"settings"`, one key per command name)
when you change them on the console, over RCON or from a cfg file, and are applied again when the
match plugin loads after a server restart:

- `ru_webhook_url`, `ru_heartbeat_url`, `ru_match_token`, `ru_admins_url`,
  `ru_admins_refresh_seconds` (`<setting> clear` forgets them)
- `ru_cfg_exec_enable`, `ru_warmup_enable`, `ru_warmup_message_html`, `ru_warmup_respawn`,
  `ru_warmup_ignore_win_conditions`, `ru_warmup_roundtime_minutes`, `ru_warmup_startmoney`,
  `ru_warmup_maxmoney`, `ru_warmup_buy_anywhere`, `ru_warmup_infinite_ammo`
- `ru_demo_recording_enabled`, `ru_demo_path`, `ru_demo_name_format`, `ru_demo_upload_url`,
  `ru_demo_upload_method`, `ru_demo_upload_attempts`, and the headers from
  `ru_demo_upload_header` (saved together as one key, `ru_demo_upload_headers`)
- `ru_series_end_kick_delay_no_demo`, `ru_series_end_kick_delay_demo_no_upload`,
  `ru_series_end_kick_delay_demo_upload`

Not saved: `ru_dev_bots_scrim` (debug only; `ru_dev_bots_scrim cfg` goes back to readyup.cfg) and
the read-only `ru_demo_status` / `ru_match_stats`.

How the value is chosen, lowest to highest:

1. The built-in default. None of these settings has a `readyup.cfg` key.
2. The saved value, applied when the plugin loads at server start.
3. Any later command. That includes lines in `server.cfg` or other cfg files the server runs after
   Ready Up loads, so a setting kept in `server.cfg` still wins on every start, as before (and is
   saved again).

Details:

- Only values that differ from the built-in default are stored. Setting a value back to its
  default removes the key.
- `<setting> default` (e.g. `ru_demo_path default`) applies the built-in default and removes the
  saved value. `ru_demo_upload_header default` and `ru_demo_upload_headers_clear` both clear all
  saved headers. `ru_warmup_message_html default` works as before and also clears the saved value.
- A rejected value (e.g. `ru_demo_path /abs/`) changes nothing and saves nothing.
- `ru_warmup_startmoney` can raise `ru_warmup_maxmoney`; both are saved then.
- `ru plugin reload match` keeps every setting in memory and does not read `state.json`.
- Saved values are plain text, like `ru_match_token`: `ru_demo_upload_url` and upload header
  values (tokens) end up in `state.json`. An empty value is stored as `""`.

## Upgrading from Postgres

Versions before this one kept admins, settings and skins in Postgres (`readyup_db.json`). Ready Up
no longer reads either. Copy the data into the JSON files once, before or right after updating:

```bash
# reads readyup_db.json next to the core:
python3 game/csgo/readyup/tools/migrate-postgres-to-json.py --csgo game/csgo
# Postgres in a docker container (psql runs inside it):
python3 game/csgo/readyup/tools/migrate-postgres-to-json.py --csgo game/csgo --docker readyup-postgres
# preview only:
python3 game/csgo/readyup/tools/migrate-postgres-to-json.py --csgo game/csgo --dry-run
```

It only reads the database and merges into existing JSON files (existing entries win;
`--prefer-db` flips that). Restart the server (or `ru plugin reload match` / `ru plugin reload
skins`) afterwards. Then `readyup_db.json` and the Postgres container can go whenever you like.

## Dev deploys

Development builds go to a test server with `scripts/dev-deploy.sh` (see
[DEVELOPMENT.md](DEVELOPMENT.md)); the old repo-root dev installer is gone.

## Ready Up config (`readyup.cfg`)

Location (next to the shim):

- `game/csgo/readyup/bin/linuxsteamrt64/readyup.cfg`

The installer only creates this file if it's missing; it never overwrites your edits (a changed default is written as `readyup.cfg.default`).

The core reads `debug`, `banner`, `chat_prefix`, `chat_debug`, `consume_ru_chat` and the
`status_http_*` keys. The match plugin (`match.so`) reads its keys (`welcome`, `ready_hud`,
`hud_*`, `admin_prefix`, `captain_prefix_*`, `consume_ready_chat`, `dev_bots_*`, `scrim_knife`,
`knife_pick_seconds`, `idle_map_refresh_hours`) from the same place, or from a `[match]` section of this file, or from
`game/csgo/cfg/ReadyUp/match.cfg` (later ones win). It re-reads them by itself when one of those
files changes.

### Idle map refresh

After a day or more of uptime on one map, player animations run in slow motion while the tick
rate stays fine (most likely float precision in the engine clock). A map load fixes it, so:

- Loading a match always changes map, also when the server is already on the match's first map.
- `idle_map_refresh_hours=12` (default; `0` = off): when no match is loaded, nobody is connected
  and the server has been on the same map for that many hours, Ready Up loads the same map again
  (a workshop map by its id). It logs `idle-refresh: ...` and tries again at most every 10 minutes
  if the map does not change.

### Prefix keys

- `chat_prefix`: prefix used for Ready Up chat replies (Ready Up adds one space after it).
- `admin_prefix`: prefix used for the “true” admin name prefix (Ready Up adds one space after it).

Values support readable **ChatColors tokens** (CounterStrikeSharp-style), for example:

```text
chat_prefix=<DarkRed>[Ready Up]<Default>
admin_prefix=<DarkRed>[Admin]<Default>
```

If you want to include spaces, quote the value:

```text
chat_prefix="<Green>[PUG #1]<Default>"
```

### Welcome screen

- `welcome=1` (default): the first time a player joins T or CT on a map, Ready Up shows
  them (only them) a center-screen welcome card for ~5 seconds (then the ready HUD takes
  over) with the brand header, the build version, their name, their team and the current
  Ready Up mode. Team switches later on the same map do not show it again. Set `welcome=0`
  (or env `READYUP_WELCOME=0`) to disable.
- `hud_brand=Auto Tournament` (default) and `hud_logo_url=` (default empty = no image):
  the header of the welcome card and the ready HUD is `<img src='hud_logo_url'>` (when
  set) followed by `hud_brand`. Admins can check what the CS2 client renders (font
  classes, PNG/SVG images, unicode) with `.ru hudtest 1..7`; the variant is shown only to
  the admin who typed it, for ~10 seconds.
- It needs `LegacyGameEventListener` from `gamedata/engine-surface.json` (resolved and
  anchor-verified at load) and the RTTI-verified game event manager. If either is missing,
  Ready Up logs `welcome: per-client center HTML unavailable ...` once and skips it.


### Ready HUD

- `ready_hud=1` (default): during scrim warmup and match warmup every player gets a
  small center panel (only for them): `X/Y ready`, each player per side with ✔/✖
  (CT blue, T orange; your own name in bold) and a `.r` / `.ur` hint. It waits while
  the welcome screen is up, hides once the match goes live, and shows the knife
  side-pick prompt after a knife round. `ru_warmup_message_html` text (if set) is
  shown as its last line. Same `LegacyGameEventListener` requirement as the welcome screen.
- The HUD is the ready-up UI: it stays up from joining until the match goes live, so
  the periodic "type .r" chat reminders are only sent when it cannot be shown. Chat
  still announces state changes (warmup start, countdown, knife winner, LIVE).
- CS2's own warmup is never used (its WARMUP text would replace the panel): Ready Up
  emulates warmup (respawn, no round end, buy anywhere), sets `mp_warmuptime 0` /
  `mp_warmup_pausetimer 0` on map start, and ends CS2's warmup whenever the server logs
  `World triggered "Warmup_Start"` (or fires `round_announce_warmup`) while idle, in
  scrim/match warmup or in the knife round.

### Knife round

- Real matches: `map_sides: "knife"` for a map. Scrims: `scrim_knife=1` (default).
- After everyone is ready: `exec ReadyUp/knife.cfg` (+ overrides so the emulated
  warmup is undone, and `mp_logdetail 3` for the round), restart, knife round.
- Works from server log lines alone (engine events are used when they arrive):
  the round end is read from `Team "CT"|"TERRORIST" triggered "SFUI_Notice_*"`.
  `CTs_Win`/`Terrorists_Win` = that side eliminated the other. Any other end
  (time ran out, draw) is decided by players alive, then HP left (from `attacked`
  log lines), then a coin flip.
- Any player of the winning team types `.stay` / `.switch` (or `.ct` / `.t`);
  admins can use `.ru side ...`. Window: match `knifeDecisionSeconds` (default 60),
  scrims `knife_pick_seconds` (default 60). No pick = stay. Winning side with no
  humans (bots only) = stay after 3s.
- Then `mp_swapteams` (if switching), `exec ReadyUp/live.cfg`, `mp_restartgame 1`,
  and LIVE on the next `Round_Start`.

### Reloading config

- In-game: `.ru reload` (admin-only)
- Server console: `.ru reload` (always allowed)

## MAT webhook / heartbeat

Ready Up can forward a **core subset** of MatchZy-style events to Auto Tournament.

MAT also stores the Ready Up `plugin_version` reported by heartbeat and shows it on the **Servers** page (as an `RU v...` chip).

### 1) Verify connectivity (from the game server host)

MAT exposes a reachability helper:

- `GET /api/events/test`

Example:

```bash
curl -sS https://mat.example.com/api/events/test
```

### 2) Configure auth token (Bearer)

Ready Up reuses the same token for match config loading **and** webhook posting:

```text
ru_match_token <token>
ru_match_token clear
```

### 3) Configure webhook base URL

Set a base “events” URL (no match/server identifier at the end):

```text
ru_webhook_url https://mat.example.com/api/events
ru_webhook_url clear
```

Ready Up will POST to:

- `<baseUrl>/<match_slug>` when a loaded match provides a `slug`, otherwise
- `<baseUrl>/<matchid>`

### 3.5) Configure allocator heartbeat URL

MAT uses a dedicated heartbeat endpoint to track whether a server is **configured** and **allocatable**.

Set the full heartbeat URL (including server ID in the path):

```text
ru_heartbeat_url https://mat.example.com/api/servers/<serverId>/heartbeat
ru_heartbeat_url clear
```

Ready Up will POST a **small** JSON payload about every ~5 seconds while set:

- `status`: `idle|loading|warmup|live|postgame|error`
- `ready_for_allocation`: boolean
- optional `match_slug` / `matchid`

### 4) Load match config from URL (seeds match context)

```text
ru match load <url>
```

The response can be either:

- a raw `MatchConfig` object, or
- a `MatchResponse` wrapper containing `{ id, slug, config: { ... } }`

When this succeeds, Ready Up stores:

- `matchid`
- optional `slug`
- `team1.name` / `team2.name`
- roster mapping derived from `team1.players` and `team2.players` (Steam64 string keys)
- optional spectators whitelist derived from `spectators.players`
- optional `map_sides` array used for team enforcement (`team1_ct` / `team2_ct`)

## Whitelist + team enforcement (when match loaded)

When a match is loaded (match context exists), Ready Up enables **whitelist mode**:

- **Whitelist**: only SteamIDs present in `team1.players`, `team2.players`, or `spectators.players` are allowed to stay connected. Others are kicked shortly after they appear in server identity tracking.
- **Admins**: server admins are never kicked by whitelist enforcement (even if not in the match roster).
- **Team enforcement**: Ready Up forces `jointeam` so roster players can only join their allowed team.
  - Mapping uses `map_sides[map_number-1]` when present:
    - `team1_ct` ⇒ team1=CT, team2=T
    - `team2_ct` ⇒ team1=T, team2=CT
  - If `map_sides` is missing/unknown, Ready Up defaults to **team1=CT**.

## Practice + custom warmup (no built-in CS2 warmup)

Ready Up maintains its own lightweight mode state machine and can display a **non-interactive CenterHtml banner** to players.

### Commands

- **Player commands (in-game chat)**:
  - `.r` (toggle ready/unready)
  - `.ready`
  - `.unready` / `.ur`
  - `.pause` / `.p`
  - `.unpause` / `.up` (requires both teams)
  - `.gg`
  - `.ff` / `.forfeit` (captain-only; captains come from match config)
- **Mode control (server console / RCON)**:
  - `ru mode` (prints current mode)
  - `ru mode idle`
  - `ru mode practice`
- **Admin match controls (server console / RCON)**:
  - `ru start` (force start live rules regardless of ready)
  - `ru restart` (restart and return to match warmup)
  - `ru end` (force end: emits `series_end` with winner=none, clears match context, resets server)
- **Match ready-up gate (server console / RCON)**:
  - `ru_warmup_enable 0|1` (default `1`). Despite the name this is more than the banner:
    - `1`: a loaded match waits in `match_warmup` until every roster player is ready, then
      plays the knife round (if `map_sides` says so) and goes live.
    - `0`: a loaded match skips ready-up entirely. No warmup rules, no ready panel or banner,
      no knife round; Ready Up goes `match_live` on the next round start. Use it only when something else starts the match.
    - Scrims (no match loaded) are not affected.
- **Warmup banner settings (server console / RCON)**:
  - `ru_warmup_message_html <html...>`
    - token expansion: `{ready_count}` / `{connected_count}` / `{total_count}`
  - `ru_warmup_message_html default` (restore default message)
  - `ru_warmup_respawn 0|1` (toggles `mp_respawn_on_death_ct/t`)
  - `ru_warmup_ignore_win_conditions 0|1` (toggles `mp_ignore_round_win_conditions`)
  - `ru_warmup_roundtime_minutes <1..120>` (sets `mp_roundtime*`)
  - `ru_warmup_startmoney <0..60000>` (sets `mp_startmoney`)
  - `ru_warmup_maxmoney <0..60000>` (sets `mp_maxmoney`)
  - `ru_warmup_buy_anywhere 0|1` (toggles `mp_buy_anywhere`)
  - `ru_warmup_infinite_ammo 0|1` (toggles `sv_infinite_ammo`)
    - `1` sets `sv_infinite_ammo 2` (infinite ammo with reload)
- **Mode cfg exec (optional, MatchZy-style)**:
  - `ru_cfg_exec_enable 0|1`
  - When enabled, Ready Up will:
    - `exec ReadyUp/warmup.cfg` when applying warmup rules
    - `exec ReadyUp/live.cfg` when applying live rules
    - `exec ReadyUp/prac.cfg` when applying practice rules
    - `exec ReadyUp/knife.cfg` when applying knife-only rules
    - `exec ReadyUp/idle.cfg` once when entering idle mode
  - Templates live in this repo at `cfg/ReadyUp/` and should be copied to your server at `game/csgo/cfg/ReadyUp/`

### Behavior

- When `ru match load` succeeds, Ready Up enters **match_warmup** and begins showing a CenterHtml banner to roster players who are **not ready** (with `ru_warmup_enable 0` there is no ready-up: see above).
- When the first `round_started` is observed in logs, Ready Up transitions to **match_live**.

### MAT integration

If you run Auto Tournament, you typically don’t set these manually. MAT can push them live via RCON:

- `POST /api/rcon/readyup/mode`
- `POST /api/rcon/readyup/practice`
- `POST /api/rcon/readyup/idle`
- `POST /api/rcon/readyup/settings`

### Events sent (core)

- Allocator heartbeat: `POST` to `ru_heartbeat_url` every ~5s (when configured)
- `series_start` (after `ru match load` parses match context)
- `match_paused` / `unpause_requested` / `match_unpaused`
- `match_forfeit` (captain-only)
- Best-effort from server console logs:
  - `player_connect`
  - `player_disconnect`
  - `round_started`
  - `round_end`
  - `map_result` (on map load log lines)
  - `series_end` (when a different match is loaded)

## How loading works (high level)

Ready Up is a `libserver.so` bootstrap:
- CS2 loads Ready Up first because `gameinfo.gi` includes `Game csgo/readyup`
- Ready Up loads Valve’s real server module `game/csgo/bin/linuxsteamrt64/libserver.so`
- Ready Up forwards exports and installs optional hooks (chat routing, admin prefix, etc.)

## Override the real Valve module path

If you need to force a specific real server binary:

```bash
READYUP_REAL_SERVER_PATH=/full/path/to/libserver.so
```

