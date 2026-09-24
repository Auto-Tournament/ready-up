# Install

## Supported setup

Ready Up runs on Linux dedicated servers (`linuxsteamrt64`). Release builds need nothing on
the host beyond glibc 2.31+ (OpenSSL, libpq and libcurl are linked in).

Metamod can run alongside Ready Up. In `gameinfo.gi`, Metamod's `Game csgo/addons/metamod`
line stays first, Ready Up's `Game csgo/readyup` goes directly below it, and both stay above
`Game csgo`. (Ready Up above Metamod makes Ready Up load Metamod, which recursed at startup.)

## Install from GitHub release zip

1. Download `readyup-<version>-linuxsteamrt64.zip` from [Releases](https://github.com/Auto-Tournament/ready-up/releases/latest).
2. Extract it into `game/csgo`. You should end up with `game/csgo/readyup/bin/linuxsteamrt64/libserver.so`.
3. Add Ready Up to `gameinfo.gi`:

   ```bash
   cd game/csgo
   python3 readyup/tools/patch_gameinfo.py gameinfo.gi --game csgo/readyup
   ```

   If you have `gameinfo_branchspecific.gi`, run it on that file too. The patcher is
   idempotent, fixes a misplaced or duplicated entry, and writes a `.bak.<timestamp>` copy
   whenever it changes the file. `--check` only reports (exit 3 = needs patching).
4. Restart the server.

CS2 updates rewrite `gameinfo.gi`, so run the patcher again after each one.

The zip only contains files Ready Up owns, so extracting a newer one over an install keeps
your `readyup.cfg`, `readyup_db.json` and `cfg/` edits:

- `readyup/bin/linuxsteamrt64/libserver.so` (the shim)
- `readyup/bin/linuxsteamrt64/engine-surface.json` (signatures + identity anchors, also embedded in the shim)
- `readyup/bin/linuxsteamrt64/readyup.cfg.example`
- `readyup/tools/patch_gameinfo.py`, `readyup/tools/install.sh`
- `readyup/cfg-templates/ReadyUp/*.cfg` (mode cfgs, only used when cfg exec is enabled)
- `readyup/VERSION`, `README.md`, `INSTALL.md`, `LICENSE`, `BUILD_INFO` (commit + the CS2 build it was verified against)

### Optional: `readyup/tools/install.sh`

Does steps 2-3 for one or more servers, or for the `game/csgo` it was extracted into:

```bash
readyup/tools/install.sh --dry-run /path/to/cs2   # preview
readyup/tools/install.sh /path/to/cs2 /path/to/other-cs2
```

It installs the shim by copy-then-rename (a running server keeps its loaded copy; the old
one is kept as `libserver.so.prev`), creates `readyup.cfg` from the example only if missing,
copies missing mode cfgs to `game/csgo/cfg/ReadyUp/`, and patches `gameinfo.gi` /
`gameinfo_branchspecific.gi`. It never stops, starts or attaches to servers, never touches
databases or `readyup_db.json`, and never uses sudo. Run it as the server's user.

## Dev install from a checkout (`./install.sh`)

> [!WARNING]
> The repo-root `install.sh` is a **dev loop** tool for the cs2-server-manager test box: it
> builds, **stops** `csm` server `$CSM_SERVER_ID`, **wipes Ready Up DB state**
> (`ru_active_*` settings, `readyup_admins`) and attaches the console. Never point it at a
> production server, and it is never shipped in the release zip.

```bash
sudo ./install.sh /path/to/cs2/root
```

## Ready Up config (`readyup.cfg`)

Location (next to the shim):

- `game/csgo/readyup/bin/linuxsteamrt64/readyup.cfg`

The installer will **only create this file if it’s missing** (it will not overwrite admin edits).

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
- **Warmup banner settings (server console / RCON)**:
  - `ru_warmup_enable 0|1`
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

- When `ru match load` succeeds, Ready Up enters **match_warmup** and begins showing a CenterHtml banner to roster players who are **not ready**.
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

