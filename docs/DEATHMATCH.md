# Deathmatch (`plugins/deathmatch`)

Free for all and team deathmatch on CS2's own deathmatch game mode, with Ready Up's rules on
top: a kill limit and / or a time limit, a live leaderboard, spawn protection, headshot only and
weapon rounds. Ships as `plugins/deathmatch.so` (component `deathmatch`, zip
`ready-up-deathmatch-<v>-linuxsteamrt64.zip`) and in the **Full** bundle only. It does nothing
until an admin switches it on.

## Commands

`ru deathmatch ...` and the short `ru dm ...` on the console / RCON, `.ru dm ...` in chat.

| Command | Who | What |
|---|---|---|
| `dm ffa [map]` | admin | Free for all: everyone is an enemy (`mp_teammates_are_enemies 1`). |
| `dm tdm [map]` | admin | Team deathmatch: CT vs T, a kill of the other team scores for the killer and their team (`mp_teammates_are_enemies 0`, `mp_dm_teammode 1`). |
| `dm off` | admin | Back to normal: competitive (`restore_game_type` / `restore_game_mode`), the current map loads again, the match plugin goes back to idle (and to scrim warmup when players join, if enabled). |
| `dm status` | anyone | Mode, map, kill limit, time left, team score, leader, weapon round. |
| `dm top` | anyone | Top 5 (kills, deaths, headshot %) and your own rank. |
| `dm hud` | players | Your leaderboard panel off / on. |

`map` is anything `.ru map change` takes: a map name (`de_dust2`), a Workshop id, `ws:<id>`,
`workshop/<id>[/<name>]` or a pasted Workshop link.

### Switching modes and maps

CS2 reads `game_type` / `game_mode` when a map loads, so entering deathmatch always loads a map:
`dm ffa|tdm` sets `game_type 1`, `game_mode 2` and then loads

1. the map given, else
2. the mode's **default map** (the essentials plugin: `.ru map default ffa aim_map`, see below),
   else
3. the current map again.

Switching between `ffa` and `tdm` while a deathmatch map is loaded, and staying on it, needs no map
load: the rules switch and the game restarts (`mp_restartgame 1`). The Workshop download bar of the
essentials plugin shows while a Workshop map downloads.

`dm off` puts back `mp_teammates_are_enemies 0`, `mp_dm_teammode 0`, `mp_damage_headshot_only 0`,
`mp_ignore_round_win_conditions 0`, the normal loadout cvars and `mp_buy_allow_guns 255`, sets
`game_type` / `game_mode` to `restore_game_type` / `restore_game_mode` (0 / 1, competitive) and loads
the current map again, so `gamemode_competitive.cfg` resets the rest.

## Rules

Each map start (and once more 3 s later, after the map's own configs) sends, on top of
`gamemode_deathmatch.cfg`:

| Cvar | Value |
|---|---|
| `mp_teammates_are_enemies` | 1 (ffa) / 0 (tdm) |
| `mp_dm_teammode` | 0 (ffa) / 1 (tdm): CS2's team deathmatch scoring and visuals |
| `mp_respawn_immunitytime` | `spawn_protection_seconds` |
| `mp_damage_headshot_only` | `headshot_only` |
| `mp_ignore_round_win_conditions 1`, `mp_roundtime 60`, `mp_timelimit 0`, `mp_maxrounds 0` | CS2's round / map timers never end the game: the limits below do |
| `mp_warmup_end` | no CS2 warmup; the game starts at once |

Kills come from the `player_death` event. FFA: any kill of another player counts. TDM: only kills
of the other team count, for the killer and their team. Suicides, world damage and team kills only
add a death. Bots play and score like everyone else.

**End of a game:** the first player (ffa) / team (tdm) to reach the kill limit wins, or, when the
time limit runs out, the player / team with the most kills (equal kills at the top: a draw). The
winner is announced in chat and on a winner card in everyone's center panel
(`RU_HTML_PRIO_NOTICE`) for `end_delay_seconds`; then the next game starts with
`mp_restartgame 1` (or, with `restart=reload`, the map loads again). A game also starts over
when CS2's warmup ends or an admin restarts the game (`Match_Start`).

**Leaderboard:** every `hud_interval_ms` each player gets a small panel at `RU_HTML_PRIO_HUD`
(the same level as the ready HUD; welcome cards, votes and the download bar go over it): mode,
kill limit, time left, the weapon round, the team score (tdm), the top 5 (your row in green) and
your rank when you are not in the top 5. Names are HTML-escaped and cut to 16 characters.

**Weapon rounds:** with `weapon_rounds` and `weapon_round_every_minutes` set, every N minutes the
next weapon of the list is handed out for `weapon_round_seconds`: the default-loadout cvars
(`mp_ct_default_primary` / `mp_t_default_primary`, or the secondary slot for pistols) give it to
everyone **on their next spawn**; `weapon_round_restrict_buy=1` also sets `mp_buy_allow_guns 0`
so nobody buys another gun meanwhile. Afterwards the normal loadout comes back (`mp_buy_allow_guns
255`). Giving the weapon to living players at once would need an engine call Ready Up does not
have yet (a follow-up).

## Config

`cfg/ReadyUp/deathmatch.cfg` (or a `[deathmatch]` section in `readyup.cfg`), read at every game
start. The shipped template is all comments: the defaults below apply.

| Key | Default | |
|---|---|---|
| `kill_limit_ffa` | 30 | first player to N kills wins (0 = no kill limit) |
| `kill_limit_tdm` | 100 | first team to N kills wins (0 = no kill limit) |
| `time_limit_minutes` | 10 | then the leader wins (0 = no time limit) |
| `spawn_protection_seconds` | 2 | `mp_respawn_immunitytime` (0-30) |
| `headshot_only` | 0 | 1 = `mp_damage_headshot_only 1` |
| `hud` | 1 | leaderboard panel |
| `hud_interval_ms` | 1000 | resend interval; 0 = every tick (steadier panel, see [HUD.md](HUD.md)) |
| `end_delay_seconds` | 10 | winner card, then the next game (3-120) |
| `restart` | `restartgame` | `restartgame` (mp_restartgame 1) or `reload` (the map loads again) |
| `weapon_rounds` | (none) | e.g. `weapon_deagle,weapon_awp,weapon_ssg08` (`weapon_` may be left out) |
| `weapon_round_every_minutes` | 0 | 0 = no weapon rounds |
| `weapon_round_seconds` | 60 | length of a weapon round (at most the period) |
| `weapon_round_restrict_buy` | 0 | 1 = `mp_buy_allow_guns 0` during a weapon round |
| `restore_game_type` / `restore_game_mode` | 0 / 1 | what `dm off` switches back to (0 / 2 = wingman) |

A bad value keeps the default and logs `deathmatch.cfg: <key>=... is not valid`.

## Default maps (essentials)

The essentials plugin keeps one default map per mode in
`csgo/readyup/plugins/essentials/default_maps.json`:

```json
{"version": 1, "maps": {"ffa": "aim_map", "tdm": "de_dust2", "practice": "workshop/3084291314"}}
```

`.ru map defaults` lists them (ffa, tdm, practice, warmup, retakes and any other key), `.ru map
default <mode> <map|workshop id|link>` sets one (admin), `.ru map default <mode> clear` removes it.
The file is re-read when it changes. Other plugins read it through `readyup.essentials.v1`
(`core/include/readyup/essentials_iface.h`: `default_map(mode)`, and `load_map(entry)` for a map
change with the download bar). Only `ffa` and `tdm` are used today (by this plugin); the other keys
are there for the practice / warmup / retakes flows to pick up. The platform is meant to push this
table over the fleet link later (not built yet).

## With the match plugin

While deathmatch is on, the match plugin is in its **external** mode (`readyup.match.v1`
`set_external_mode("deathmatch")`, v1.5): `ru_mode` `external`, `/status` summary `mode:
"external"`, `phase: "deathmatch"`; no scrim warmup, no ready HUD, no idle cfg, no practice rules,
no round-termination suppression, and CS2's own warmup is left alone. `.r` answers "ready-up is not
used in deathmatch mode", `.help` points at `.ru help dm`.

- `dm ffa|tdm` is refused while a match is loaded or practice is on (`.prac` first); `.prac` is
  refused while deathmatch is on (`.ru dm off` first).
- `.ru mode idle` ends deathmatch: the plugin notices within half a second, puts its cvars back,
  switches to competitive and loads the map again.
- A match load (`ru match load`, a fleet assignment) ends it too. The match plugin sets
  `game_type 0` / `game_mode 1` before its own map change, so the match is competitive.
- Without the match plugin, deathmatch keeps its on / off state itself.

`ru plugin reload deathmatch` keeps the mode, the game clock and the scores.

## Fleet (platform)

Nothing new on the wire yet. Today the platform can switch a server's plugins with `plugins.set`
(`{enable: ["deathmatch"]}`) and run the commands with the root-only `exec`
(`{command: "ru dm ffa aim_map"}`). The natural next step is a `deathmatch.set` command next to
`practice.set` (`{mode: "ffa" | "tdm" | "off", map?}`, `rejected unsupported` without
deathmatch.so, `rejected bad_phase` while a match is loaded) and a `default_maps` push (the table
above) in `server.config`; neither is implemented.

## Log lines

`deathmatch: ffa on aim_map (argument map)`, `deathmatch: game started (tdm, map start)`,
`deathmatch: game over: ...`, `deathmatch: weapon round weapon_deagle`, `deathmatch: off (...)`.
`ru selftest` shows a `deathmatch` INFO line.
