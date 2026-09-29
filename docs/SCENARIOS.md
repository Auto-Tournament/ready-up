# Scenarios: replay a pro round

A scenario is one recorded round from a CS2 demo. In practice mode you pick a scenario and one of
the ten players in it. You spawn where that player stood, with their health, armor, money and
weapons. Bots take the other slots and replay the round: each bot follows its player's recorded
path tick by tick. When a bot spots you, or you hurt it, it stops following the path and plays
like a normal bot.

Scenarios are made from demos with an offline converter (`tools/scenario/ru_scenario.py`). The
server can also run the converter on a demo it already has (`ru scenario convert`). Ready Up ships
four seed scenarios from a public pro demo; see [Seed scenarios](#seed-scenarios).

## Commands

Scenarios only work in practice mode (`.prac`), and never under the valve ruleset. Anyone in
practice mode can use them, like the other practice tools.

| Chat | What it does |
|---|---|
| `.scen list` | Scenarios for the current map. `.scen list all` shows every map. `.scenarios` does the same. |
| `.scen info <id>` | Shows the players, length, grenades, bomb and source. |
| `.scen load <id> [player] [start]` | Starts a scenario. You can shorten this to `.scen <id> ...`. |
| `.scen r` | Plays the same scenario again from the same start. Bots are reused when the teams still match. |
| `.scen stop` | Ends the scenario. Bots are kicked and `prac.cfg` is run again. |

- `<id>`: the scenario id, or any unique part of it (`b_exec` finds `nuke_vitality_b_execute`).
- `[player]`: a name (`zywoo`, or a unique part of one), the number from `.scen info`, or a
  SteamID64. Leave it out to get the first player on your team who is alive at the start. You have
  to be on that player's team already. Use `watch` for bots only (all ten players are bots).
- `[start]`: where to start. `25`, `25s` or `1:05` means seconds after freeze time ended. `t12345`
  is a tick in the original demo. Leave it out to use the scenario's own start (usually 0:00).
  `.scen load <id> 1:05` works without a player.

The console and RCON take the same subcommands as `ru scenario ...`, and admins can use
`.ru scenario ...` in chat. The console has no player of its own. If exactly one human is on the
server, they play; otherwise the scenario runs in watch mode. The console also has:

| Console / RCON | What it does |
|---|---|
| `ru scenario status` | Shows the running scenario, the clock, and each bot: scripted, AI or dead, with its position next to the recorded one. |
| `ru scenario rounds <demo.dem>` | Lists the rounds in a demo on the server. |
| `ru scenario convert <demo.dem> <round[,round]> [start\|-] [id]` | Converts demo rounds into scenarios, in the background. |

## What happens during a replay

1. **Load.** Ready Up kicks all bots and sets the scenario cvars: `bot_stop 1`,
   `bot_ignore_players 1`, no respawn, `buddha 0`, `sv_infinite_ammo 0` and
   `mp_ignore_round_win_conditions 1`. It adds one bot for each recorded player who is alive at the
   start, except the one you play. Then it runs `mp_restartgame 1`, so everyone is alive.
2. **Setup.** Everyone is moved to their recorded position. Bots also get their recorded view
   angles. Everyone gets the recorded health, armor, helmet, defuser, money and weapons. Weapons are
   removed first; the knife stays. You get a chat line with the pro's view angles as a `setang`
   command, because the server cannot turn your view for you.
3. **Replay.** On every tick each scripted bot is moved along its path, with its view angles and
   horizontal speed. Positions are sampled every 4 ticks (16 per second) and blended in between.
   When a player died in the recording, their bot dies at that moment (`bot_kill`), except when the
   killer was the player you are playing. Bomb plants, defuses and explosions show up in chat.
4. **Handoff.** A bot hands over to the bot AI when it spots you (the same check the radar uses) or
   when you damage it. The first handoff turns the AI on for all bots (`bot_stop 0`,
   `bot_ignore_players 0`). Bots that were handed over fight like normal bots. The other bots keep
   following their paths, but they can now aim and shoot. While both bots are still scripted,
   damage between them is undone, so the recording decides who dies.
5. **End.** When the recording ends, every bot is handed over. `.scen r` plays it again.

A scenario also stops when practice mode is turned off, when the map changes, or when the plugin
is reloaded.

### What is approximate in v1

- **Grenades are re-thrown, not re-aimed.** Each grenade is spawned at its recorded tick from its
  recorded spawn point with its recorded velocity (ru_api `grenade_spawn`, API 1.12), thrown by the
  pro's bot (else a live bot of the same team, else nobody), so it flies, bounces and detonates like
  the original and damage is credited to that bot. The bot does not do a throw animation. Utility
  that is already out at the start (smokes, fires, decoys) is dropped where it landed. On a CS2
  build where a grenade type's gamedata does not verify (or without the practice gamedata
  `engine-surface.practice.json`) that grenade is skipped and logged
  (`practice: scenario 0:24 smoke by apEX (not thrown: grenade spawn not available ...)`).
- **The bomb is not planted.** Plants, defuses and explosions only go to chat.
- **Bots are moved, not steered.** A scripted bot is teleported along its path every tick, with
  its speed set so the legs animate. It does not crouch, jump, reload or switch weapons as the pro
  did. Its view angles are set, but the bot does not shoot while it is scripted.
- **Handoff is one-way and turns the AI on for everyone.** CS2's bot AI is switched on and off with
  server-wide cvars, not per bot.
- **Recorded health changes are not replayed.** A bot keeps the health it had at the start until
  someone damages it or it dies on schedule.
- **You play from the pro's position, not their view.** Type the `setang` line from chat into your
  console to look where they looked.
- **You have to be on the pro's team.** Ready Up cannot move a human to another team from the
  server.

## Converting demos

The converter needs Python 3.8 or newer and [demoparser2](https://github.com/LaihoE/demoparser)
(MIT licence). Install it with `pip install demoparser2`. Ready Up does not bundle it.

```bash
python3 tools/scenario/ru_scenario.py rounds match.dem
python3 tools/scenario/ru_scenario.py convert match.dem --round 6 \
    --id nuke_b_execute --title "B execute" --start 0:20 \
    --event "Some Cup 2026" --match "Team A vs Team B, map 2" --url "https://example.org/match" \
    --out csgo/readyup/plugins/practice/scenarios/
python3 tools/scenario/ru_scenario.py validate csgo/readyup/plugins/practice/scenarios/*.json
```

- `--round 6` or `--round 6,9,12` converts one or more rounds; you get one file per round. Round
  numbers are what `rounds` shows: the Nth time freeze time ended.
- `--start` sets the scenario's default start (seconds, `1:05`, or a demo tick like `t12345`). The
  whole round is always recorded, so players can still pick any start with `.scen load`.
- `--sample N` sets the ticks between path samples (default 4). A 100-second round is about
  450 KB at 4.
- `--strict` rejects the file when the attribution is missing (`--event` or `--match`, plus
  `--url`). Scenarios in this repo must use it.

Put the files in `csgo/readyup/plugins/practice/scenarios/`. The plugin reads new and changed
files on the next `.scen list` or `.scen load`; no reload is needed.

### On the server

`ru scenario convert <demo.dem> <round[,round]> [start|-] [id]` runs the same converter on the
server, in the background, and writes into the scenarios folder. It looks for the demo first in
`csgo/readyup/plugins/practice/demos/`, then in `csgo/` (where `tv_record` and Ready Up's own demo
recording write). The demo name is a path relative to one of those folders. Absolute paths and
`..` are refused. `ru scenario rounds <demo.dem>` lists the rounds first. Only the console and
admins may convert.

The server needs `python3` with demoparser2. Settings go in `cfg/ReadyUp/practice.cfg`:

| Key | Default | |
|---|---|---|
| `scenario_python` | `python3` | The Python that has demoparser2. |
| `scenario_pythonpath` | (none) | Extra `PYTHONPATH` for it, for example a `pip install --target` folder. |
| `scenario_converter` | `readyup/tools/scenario/ru_scenario.py` | The converter the practice component installs. |

The converter is a separate process (`posix_spawn`, no shell), so a slow demo never holds up the
server. Why not a C++ parser in the plugin? A CS2 demo is a stream of protobuf packets with
entity deltas that follow the game's send tables. Parsing that correctly is a large project, and it
would have to be updated after every CS2 update. demoparser2 is maintained, MIT-licensed and fast.
A command that runs it keeps working on a server without the platform, and servers without Python
can still convert demos offline and copy the JSON over.

### Platform upload (not built yet)

Uploading a `.dem` in the Auto Tournament web UI is out of scope for v1. The intended hook is:
the platform runs the same converter (or its own copy of `build_scenario`), shows `rounds` in the
UI so the user can pick rounds and a start, and sends the resulting JSON to the server over the
fleet link. On the server it goes into `csgo/readyup/plugins/practice/scenarios/`, the same folder
as everything else. Nothing in the plugin has to change: it reads whatever is in that folder.

## File format (version 1)

One JSON object per file, named `<id>.json`. Times (`t`) are ticks after freeze time ended. The
plugin refuses a file with another `format` or `version`.

```jsonc
{
  "format": "readyup.scenario", "version": 1,
  "id": "nuke_vitality_b_execute",          // [a-z0-9_-]{1,64}, same as the file name
  "title": "Vitality T execute onto B",
  "map": "de_nuke",
  "tickrate": 64,                           // demo ticks per second
  "sample_ticks": 4,                        // track samples every 4 ticks
  "round": 6,                               // Nth freeze time end in the demo
  "freeze_end_tick": 38336,                 // demo tick of t = 0 (so "t38400" starts can be given)
  "length": 6998,                           // ticks recorded (to the round_end event)
  "default_start": 0,
  "winner": "T", "reason": "bomb_exploded",
  "source": {                               // attribution: required for shipped scenarios
    "event": "...", "match": "Team Vitality vs Team Spirit, map 2 (de_nuke)",
    "teams": {"T": "Team Vitality", "CT": "Team Spirit"},
    "url": "https://doi.org/...", "demo": "vitality-vs-spirit-m2-nuke.dem", "note": "..."
  },
  "converter": {"name": "ru_scenario.py", "version": "1", "parser": "demoparser2 0.42.0"},
  "players": [                              // up to 10: T first, then CT
    {
      "name": "ZywOo", "steamid": "7656...", "team": "T",
      "alive_until": 2140,                  // tick they died, or null
      "track": [x, y, z, yaw, pitch, ...],  // one group of 5 per sample, from t = 0 until death / end
      "state": [                            // from t on; the first entry is t = 0
        {"t": 0, "hp": 100, "armor": 100, "helmet": true, "defuser": false, "money": 3150,
         "items": ["weapon_knife", "weapon_ak47", "weapon_smokegrenade"]}
      ]
    }
  ],
  "grenades": [                             // index into players; spawn point and launch velocity
    {"t": 1536, "player": 4, "type": "smoke", // smoke flash hegrenade molotov incgrenade decoy
     "pos": [x, y, z], "vel": [vx, vy, vz], "land": [x, y, z], "land_t": 1810}
  ],
  "deaths": [{"t": 2140, "player": 4, "killer": 6, "weapon": "glock"}],   // killer -1: world / bomb
  "bomb":   [{"t": 4370, "event": "planted", "player": 2, "pos": [x, y, z]}] // planted defused exploded
}
```

- `track` positions are the player's origin (their feet) in world units. `yaw` and `pitch` are
  their view angles in degrees.
- `items` are the classnames `give` takes. Every knife becomes `weapon_knife`. Duplicates mean
  several grenades of one type.
- A grenade's `pos` is where its projectile first appeared. `vel` is the launch velocity, fitted
  from its first positions and corrected for gravity (grenades fall at 320 u/s²). `land` and
  `land_t` come from the detonate event, or from the projectile's last position when there is no
  event (molotovs).
- Additions within version 1 are new optional keys. Readers ignore keys they do not know. A change
  that old readers would misread bumps `version`.

The C++ reader is `plugins/practice/scenario.cpp` (ctest `practice_scenario`). The Python reference
is `build_scenario` / `validate` in the converter (`tests/test_scenario_converter.py`).

## Seed scenarios

All four come from one demo: Team Vitality vs Team Spirit, map 2, de_nuke. It was played on an
ESL match server; the demo does not name the event. The demo was published on figshare by Peter
Xenopoulos as test data for the awpy project
([doi:10.6084/m9.figshare.28433153.v1](https://doi.org/10.6084/m9.figshare.28433153.v1), CC BY 4.0).

| id | Round | What happens |
|---|---|---|
| `nuke_vitality_b_execute` | 6 | Vitality (T) execute onto B (lower), plant at 1:08, the bomb explodes |
| `nuke_vitality_a_take` | 10 | Vitality (T) take A (upper) with a late plant |
| `nuke_spirit_hold_fast_push` | 16 | Spirit (CT) stop a fast Vitality push in 29 seconds |
| `nuke_spirit_ct_utility` | 18 | Spirit (CT) utility shuts down a Vitality attack |

HLTV, the usual source for pro demos, sits behind a Cloudflare challenge that blocks automated
downloads. It was not bypassed; the figshare mirror was used instead.

## Contributing scenarios

1. Use a demo you are allowed to use. For a pro match, the HLTV match page or the tournament's
   own demo link is best.
2. Convert it with `--strict` and full attribution: `--event`, `--match` (teams and map number)
   and `--url` (where the demo came from). Give it a descriptive `--id` (`<map>_<team>_<what>`)
   and a one-line `--title`.
3. Run `python3 tools/scenario/ru_scenario.py validate` on the file, and keep it under 2 MB
   (`--sample 8` halves the size).
4. Add it to `plugins/practice/scenarios/` in a pull request. Say which round it is and why it is
   worth practicing. `python3 -m unittest discover -s tests` checks every file there.

**Never commit `.dem` files.** Demos are Valve game data that third parties (HLTV, tournament
organisers, FACEIT) redistribute. They are not public domain, and their terms vary. A scenario
holds only derived facts: positions, view angles, inventories, grenade and death timings, plus the
names printed in the demo. That is why only derived data is shipped, and only with attribution. If
a rights holder asks for a scenario to be removed, remove it.
