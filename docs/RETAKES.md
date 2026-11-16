# Retakes

`plugins/retakes` (`retakes.so`, Full bundle): every round the bomb is already planted on one site. The Ts hold it and the CTs retake it.

## Spawns

CS2 ships retake spawns for its own retake maps in `pak01_dir.vpk` (`maps/<map>_retake.txt`). The plugin reads them from the server's install when a map starts, so Ready Up ships none of Valve's data. Maps with spawns: de_ancient, de_ancient_night, de_anubis, de_cache, de_dust2, de_inferno, de_mirage, de_nuke, de_overpass, de_train, de_vertigo. On any other map the plugin says so in chat and waits.

## A round

- **Before it starts** (`round_prestart`): the teams. T = round(0.45 × players), at least one from two players, half at 10. Past `max_players` people wait in spectator, oldest first. Ts win `scramble_after` rounds in a row: everyone is shuffled. CTs win: the CTs who scored (kill 50, assist 25, defuse 50) attack next.
- **Start** (`round_start`): a random site. Everyone gets a different spawn of their team on it; the planter gets the most central T spawn.
- **Freeze end**: the bomb is planted where the planter stands (a `planted_c4` with the site set, ticking on `mp_c4timer`), and the game rules say a bomb is planted.

Loadouts are CS2's default-loadout cvars: T AK-47 and Desert Eagle, CT M4A1-S and Desert Eagle, armor with helmet, defuse kits for every CT. Money and buying are off.

While it runs the match plugin is in external mode `retakes` (no scrim warmup or idle cfg). A match load ends it.

## Settings

`cfg/ReadyUp/retakes.cfg`: `enabled`, `max_players` (2-10, default 9), `scramble_after` (0 = never, default 5).

Console: `ru retakes` (status), `ru retakes site a|b|random`.

## Not yet

- Only human players are placed (bots have no player slot in the API).
- The `bomb_planted` game event is not fired (the core cannot fire events): bots and CS2's own "bomb planted" voice line don't react. The round timer and HUD read the game rules.
- Loadout choice per player (`.guns`) and the HUD panels come next.
