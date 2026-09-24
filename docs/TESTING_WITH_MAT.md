# Testing ReadyUp with MAT (manual match, 2 PCs)

This runbook is for validating the **full ReadyUp flow** via **Auto Tournament** using a **manual match** and **two player machines**.

## Prereqs (must be true before testing)

- **ReadyUp console hook works** (required for MAT to load matches):
  - On the game server, run: `ru sigtest`
  - Expected: `sigtest OK`
  - If the command is unknown or doesn’t respond, ReadyUp likely failed to hook the command buffer and **MAT cannot drive ReadyUp via RCON**.

- **Server can reach MAT**:
  - The game server must be able to `GET` the match config URL that MAT serves (`/api/matches/:slug.json`).

- **MAT can RCON the server**:
  - MAT must be able to send RCON successfully (you should see successful ReadyUp init commands being sent during allocation/load).

## Manual match config requirements (MAT)

Your manual match must include (at minimum):

- `matchid`: non-zero
- `team1.players` + `team2.players`: map of `steamid64 -> playerName`
- `team1.captain_steamid64` + `team2.captain_steamid64` (required if you test knife + `.ru side`)
- `maplist`: `["de_anubis"]` (or any installed map)
- `map_sides`: one of:
  - `["knife"]` to test knife flow, or
  - `["team1_ct"]` / `["team2_ct"]` to test predetermined sides
- **Recommended (for correct OT + tie logic)**:
  - `maxRounds`: regulation total rounds (e.g. `24` for MR12)
  - `overtimeMode`: `"enabled"` or `"disabled"`
  - `overtimeSegments`: rounds per overtime half (e.g. `3` for MR3 halves)
- **Optional (damage-based tiebreak + OT cap)**:
  - `damageTiebreak`: `true|false` (when enabled, ReadyUp can resolve ties by total roster-team damage)
  - `damageTiebreakSuddenDeath`: `true|false` (when enabled and damage is tied, keep playing until a team leads)
  - `maxOvertimes`: `0..N` (max overtime blocks; each block is `2*overtimeSegments` rounds). Use `0` to disallow full overtime blocks while still allowing sudden-death if enabled.
- `cvars`: include at least a couple you can verify later, e.g.:
  - `mp_maxrounds`
  - `mp_overtime_enable`
  - `mp_overtime_maxrounds`

### Example: \"no overtime allowed\" (damage decides; sudden death on damage tie)

This configuration makes ReadyUp decide the winner by **total team damage** when regulation ends tied, without playing full overtime blocks:

```json
{
  "matchid": 123,
  "maplist": ["de_nuke"],
  "num_maps": 1,
  "map_sides": ["knife"],
  "maxRounds": 24,
  "overtimeMode": "enabled",
  "overtimeSegments": 3,
  "maxOvertimes": 0,
  "damageTiebreak": true,
  "damageTiebreakSuddenDeath": true,
  "team1": { "name": "Team 1", "captain_steamid64": "7656...", "players": { "7656...": "p1" } },
  "team2": { "name": "Team 2", "captain_steamid64": "7656...", "players": { "7656...": "p2" } },
  "cvars": {
    "mp_maxrounds": 24,
    "mp_overtime_enable": 1,
    "mp_overtime_maxrounds": 6
  }
}
```

## What MAT should send to ReadyUp

During server initialization + load, MAT should send these (via RCON):

- Persistent init:
  - `ru_webhook_url <base>/api/events`
  - `ru_heartbeat_url <base>/api/servers/<serverId>/heartbeat`
  - `ru_match_token <token>`
- Match load:
  - `ru match load <base>/api/matches/<slug>.json`

## Test steps (2 PCs)

### 1) Allocate/load the manual match via MAT

- Use MAT’s manual match flow so the match gets assigned a server and loaded.
- Expected server behavior after load:
  - ReadyUp enters **RU warmup** (`MatchWarmup`)
  - Server should **not** be using CS2 built-in warmup (ReadyUp best-effort disables it)
  - If `maplist` is provided and map differs, ReadyUp may `changelevel` to map 1

### 2) Join from both player machines

- Join from the two SteamIDs listed in the match config (one on each team).
- Expected:
  - **Whitelist enforcement**: any non-roster (and non-spectator) accounts get kicked shortly after connecting
  - **Team enforcement**: when a roster player tries to join a team, ReadyUp forces `jointeam` to the correct side based on `map_sides[0]`
  - **RU warmup banner**: unready roster players see a CenterHtml banner prompting `.r`

### 3) Ready up

- On both PCs, type `.r` (or `.ready`) in chat.
- Expected:
  - ReadyUp recognizes the commands and marks each roster player ready
  - When all roster players are **connected + ready**, ReadyUp:
    - applies **live rules**
    - applies match config `cvars{}`
    - runs `mp_restartgame 1`

### 4a) If `map_sides[0] == "knife"`

- Expected:
  - ReadyUp enters **knife mode**, restarts, and the knife round plays
  - At knife end, the winner must pick:
    - captain command: `.ru side stay|switch|ct|t`
    - console/admin override: `ru side stay|switch|ct|t`
  - After side pick, ReadyUp returns to **RU warmup** and you must **ready up again** to go live.

### 4b) If predetermined sides (`team1_ct` / `team2_ct`)

- Expected:
  - No knife round; ReadyUp transitions directly to live after the restart.

## Verifying that match `cvars{}` were applied

After ReadyUp goes live (post-ready restart), query a few cvars via RCON, e.g.:

- `mp_maxrounds`
- `mp_overtime_enable`
- `mp_overtime_maxrounds`

These should reflect the values from the match config `cvars{}` (unless blocked by server permissions or invalid cvar names/values).

## Common failure points

- **No response to `ru sigtest`**: ReadyUp didn’t hook the command buffer → RCON `ru ...` commands won’t work.
- **Players aren’t forced to teams**: `IServerGameClients::ClientCommand` hook failed → team enforcement may be broken.
- **No RU warmup banner / `.r` ignored**: chat interception path failed (but RCON may still work). Check server logs for chat-hook installation messages.

