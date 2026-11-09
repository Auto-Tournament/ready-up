# Retakes

`plugins/retakes` (`retakes.so`, Full bundle): every round the bomb is already planted on one site. The Ts hold it and the CTs retake it.

## Spawns

CS2 ships retake spawns for its own retake maps in `pak01_dir.vpk` (`maps/<map>_retake.txt`). The plugin reads them from the server's install when a map starts, so Ready Up ships none of Valve's data. Maps with spawns: de_ancient, de_ancient_night, de_anubis, de_cache, de_dust2, de_inferno, de_mirage, de_nuke, de_overpass, de_train, de_vertigo. On any other map the plugin says so in chat and waits.

## A round

- **Before it starts** (`round_prestart`): the teams. T = round(0.45 × players), at least one from two players, half at 10. Past `max_players` people wait in spectator, oldest first. Ts win `scramble_after` rounds in a row: everyone is shuffled. CTs win: the CTs who scored (kill 50, assist 25, defuse 50) attack next.
- **Start** (`round_start`): a random site. Everyone gets a different spawn of their team on it; the planter gets the most central T spawn.
- **Freeze end**: the bomb is planted on the site's plant spot (the most central T spawn), with or without a T on it (a `planted_c4` with the site set, ticking on `mp_c4timer`), and the game rules say a bomb is planted.

- **After the respawn** (`round_poststart`): everyone's weapons are taken (the knife stays) and the loadout is given: their rifle, a Desert Eagle, armor with helmet and, on CT, a defuse kit. One AWP per team goes to one of the players who asked for it, drawn at random. CS2 swaps a given M4A4 for the player's own M4A1-S pick (and Deagle for R8); the plugin sets the item back.

Bots play like everyone else. The rules cvars go in again every round: a map reload (the addons plugin's, an admin's) runs CS2's gamemode cfg over them. CS2's warmup is ended when it starts.

## Loadouts

Each player picks one rifle per side, since sides change all the time: T AK-47 or SG 553, CT M4A4, M4A1-S or AUG, and whether they want the AWP. `.guns` opens the menu (hud.so); `.guns ak|sg|m4|m4s|aug` picks a rifle straight away and `.guns awp` turns the AWP on or off. Picks are kept by SteamID in `csgo/readyup/plugins/retakes/guns.txt`. Without a pick: AK-47 and M4A1-S.

A pick within 10 seconds of the round's loadout, before the player's first shot, swaps their rifle at once; later picks count from the next round. Money and buying are off.

## HUD

With hud.so and the HUD addon (chat lines without them):

- **Freeze time**, centre: "Retake Site B!" for CTs, "Defend Site B!" for Ts, in CS2's own words, and the players on each side. When freeze time ends it becomes a small pill at the bottom of the screen for the rest of the round.
- **Waiting**, the card above the chat: players on the server out of `max_players`, how many wait, and your place in line. It stays up until you get in.
- **`.guns`**: the rifles of both sides and the AWP, the current picks marked. The buttons show CS2's own weapon icons from the HUD addon with the `wpn-*` classes (`hud/styles/at_ui.css`); an older addon shows the names only.

While it runs the match plugin is in external mode `retakes` (no scrim warmup or idle cfg). A match load ends it.

## Settings

`cfg/ReadyUp/retakes.cfg`: `enabled`, `max_players` (2-10, default 9), `scramble_after` (0 = never, default 5).

Console: `ru retakes` (status), `ru retakes site a|b|random`.

## Not yet

- The `bomb_planted` game event is not fired (the core cannot fire events): bots and CS2's own "bomb planted" voice line don't react. The round timer and HUD read the game rules.
- Full-buy rounds only: no pistol or half-buy rounds, and no grenades.
