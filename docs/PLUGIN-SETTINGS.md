# Plugin settings

The plugins' settings and their defaults are built into each plugin. Set a key in `readyup/bin/linuxsteamrt64/readyup.cfg` under a section named after the plugin, for example:

```ini
[mapvote]
maps=de_dust2, de_mirage, de_inferno
```

The platform sets some of them itself. A server that still has a `cfg/ReadyUp/<plugin>.cfg` from an older install keeps it: Ready Up reads that file first. These examples used to ship as those files.

## addons

`[addons]` in readyup.cfg.

```ini
// readyup-addons (plugins/addons): Steam Workshop addons the server downloads and mounts.
// Re-read every 5 seconds. One key=value per line; comments only on their own lines.
// The items must be Public or Unlisted on the Workshop (a Hidden item can't be downloaded).
// workshop_addons=3808995594
// Resources to load with every map (paths inside the addons, comma separated). A model that is
// not in the map's resource manifest shows as ERROR ("requested but is not in the system").
// Default (unset or "auto"): every model the addons contain. Add explicit paths after "auto,".
// precache=auto
```

## community

`[community]` in readyup.cfg.

```ini
// readyup-community (plugins/community): .rank, .top, .speclist and admin .kick. One key=value
// per line; comments only on their own lines. Every value below is the default.
//
// 0 = all of it off.
// enabled=1
// 1 = count kills and deaths during matches too (off: a match has its own stats on the platform).
// count_in_matches=0
// Kills a player needs before .rank gives them a rank.
// min_kills_to_rank=10
```

## deathmatch

`[deathmatch]` in readyup.cfg.

```ini
// readyup-deathmatch (plugins/deathmatch): the rules on top of CS2's deathmatch game mode.
// Deathmatch is off until an admin types `.ru dm ffa` / `.ru dm tdm` (docs/DEATHMATCH.md).
// Read at every game start. One key=value per line; comments only on their own lines.
// Every value below is the default; remove the // to change one.
//
// First player (ffa) / team (tdm) to this many kills wins (0 = no kill limit).
// kill_limit_ffa=30
// kill_limit_tdm=100
// Minutes per game; then the leader wins, equal kills at the top = draw (0 = no time limit).
// time_limit_minutes=10
//
// Seconds of spawn protection after each respawn (mp_respawn_immunitytime).
// spawn_protection_seconds=2
// 1 = only headshots do damage (mp_damage_headshot_only).
// headshot_only=0
// 1 = a kill refills the killer's magazine (the gun in hand; reserve ammo stays as it is).
// reload_on_kill=0
//
// Leaderboard panel (top 5 + your rank) in the center of the screen; players can hide theirs
// with .ru dm hud. hud_interval_ms = how often it is resent (0 = every tick; see docs/HUD.md).
// hud=1
// hud_interval_ms=1000
//
// After a win: the winner card for this many seconds, then the next game. restart=restartgame
// (mp_restartgame 1) or reload (the map loads again).
// end_delay_seconds=10
// restart=restartgame
//
// Weapon rounds: every weapon_round_every_minutes minutes everyone spawns with the next weapon of
// the list for weapon_round_seconds seconds (players get it on their next spawn). 1 in
// weapon_round_restrict_buy also stops buying other guns meanwhile (mp_buy_allow_guns 0).
// weapon_rounds=weapon_deagle,weapon_awp,weapon_ssg08
// weapon_round_every_minutes=0
// weapon_round_seconds=60
// weapon_round_restrict_buy=0
//
// `.ru dm off` switches the server back to this game_type / game_mode (0 / 1 = competitive,
// 0 / 2 = wingman) and loads the map again.
// restore_game_type=0
// restore_game_mode=1
```

## mapvote

`[mapvote]` in readyup.cfg.

```ini
// readyup-mapvote (plugins/mapvote): .rtv, .nominate, an end-of-map vote and .timeleft on
// community servers. Off while a match is loaded. Re-read at every map start and with
// `ru mapvote reload`. One key=value per line; comments only on their own lines.
// Every value below is the default except maps; remove the // to change one.
//
// The map pool: map names and Workshop entries (workshop/<id>/<name>), comma separated.
// Empty = CS2's Active Duty maps on a defuse map (de_, cs_); on any other map (surf, kz, aim)
// map voting stays off until you list maps here. A pool the platform gave this server (a saved
// setup) is kept in plugins/mapvote/pool.txt and wins over this line.
maps=
// enabled=1
//
// Share of the players on the server who must type .rtv to start a vote, and the minimum count.
// rtv_ratio=0.6
// rtv_min_players=1
// No .rtv in the first seconds of a map.
// rtv_delay_seconds=60
//
// Maps on a ballot (nominations first), and how long a vote runs.
// vote_options=6
// vote_seconds=20
//
// Minutes per map (0 = no time limit, votes only by .rtv); the vote starts this many seconds
// before the time runs out. "Extend" adds extend_minutes, at most max_extends times.
// map_time_minutes=30
// vote_before_end_seconds=120
// extend_minutes=10
// max_extends=1
//
// The last N maps played are left off the ballot.
// recent_maps_excluded=3
```

## midas

`[midas]` in readyup.cfg.

```ini
// readyup-midas (plugins/midas): Midas players' weapons turn gold.
// Off by default and never active under the valve ruleset. Re-read every 5 seconds.
// One key=value per line; comments only on their own lines.
//
// enabled=1
//
// Who is Midas: these players (SteamID64s, comma separated) ...
// midas_steamids=76561198000000000,76561198000000001
//
// ... and the players an admin gives it to in game: `.ru midas give <player>` (part of the name or a
// SteamID64; again = takes it back), `.ru midas take <player>`, `.ru midas` = who is Midas and why.
// Kept in csgo/readyup/plugins/midas/given.txt (one SteamID64 per line) across maps and restarts.
//
// A card in the middle of the screen for ~5 s tells a player they became Midas / lost it, at their
// next spawn or round start (everyone else gets the chat line). 0 = no cards.
// cards=1
// The texts (empty = these defaults). {stat} = ADR or kills, {value} = the number, {name} = who
// it passed to. Markup: <b>, <br>, <font color='#rrggbb' class='fontSize-m'>.
// card_title=Blessed by Midas
// card_best=You're the best player on the server ({stat} {value}). Everything you touch turns to gold.
// card_given=An admin has blessed you with the Midas touch.
// card_config=You have the Midas touch.
// card_lost=The Midas touch has left you.
// card_passed=It passed to {name}.
//
// ... and/or the best player of the map, from the match plugin's stats (scrims only by default).
// best_player=1
// Stat: adr (default) or kills. Ties: the other stat, then fewer deaths, then the current Midas.
// best_player_stat=adr
// When: round (default) = each round start once best_player_min_rounds rounds are played;
// half = at the start of every new half (the best of the map so far, for that half).
// best_player_when=round
// best_player_min_rounds=3
// 1 = also in real matches (never under ruleset valve).
// best_player_in_matches=0
//
// How gold looks. finish=auto (default): a gold paint kit through the skins plugin when skins.so
// is loaded, the render colour otherwise and on grenades / C4. finish=tint: always the
// render colour (the server can't send custom textures).
// finish=auto
// paint_kit: any items_game paint kit id; 1025 = "Gold Brick", a pattern finish that fits every gun.
// paint_kit=1025
// paint_wear: 0 = factory new ... 1 = battle-scarred.
// paint_wear=0.0
// paint_seed=0
// color: r,g,b[,a]
// color=255,200,40
// Gold models from the readyup_midas Workshop addon (the server must mount it and precache the model):
// model_<classname>=<vmdl>. White metal, tinted by `color`.
// model_weapon_ak47=models/midas/weapon_rif_ak47_gold.vmdl
// A material group of the Midas model (the readyup_midas addon has only gold, its default);
// empty = the default. Applies to newly given weapons.
// model_group=
// A name tag on Midas weapons, e.g. name_tag=Midas Touch. Default: none (the weapon shows its own
// skin name).
// name_tag=
// StatTrak counter on Midas weapons (guns and knives, not the Zeus / grenades): the Midas player's
// kills on this map (the match plugin's stats while it records, else kills since the map started),
// updated on every kill. 0 = don't touch StatTrak. A player who stops being Midas (or stattrak=0)
// gets their weapons swapped for new ones through the skins plugin, since the counter can't be
// taken off a weapon; without skins.so it stays (frozen) until the weapon is replaced.
// stattrak=1
// Dev only, with debug on: bots count as Midas players (to test gold / StatTrak without humans).
// debug_midas_bots=0
// A player's charm on a Midas-model weapon: removed by default (the custom model has no charm
// placement data, so it floated). 1 = keep it anyway.
// keep_charm=0
// Midas models for many weapons: csgo/readyup/plugins/midas/models.txt, one "<defindex> <classname>
// <model>" per line (the readyup_midas addon's midas_models.txt). model_def_<defindex>=<vmdl> here wins.
// Grenades, C4, defuser and healthshot use their item's line too; a grenade a Midas player throws
// (by its grenade: 43 flashbang, 44 HE, 45 smoke, 46 molotov, 47 decoy, 48 incendiary) and the bomb
// they plant (49 C4) keep the gold model in flight / planted. No line for an item: unchanged.
// paint_kit=skin keeps the player's own skin (and its name) under the model; the model always shows
// its new (hd) mesh, the only one the readyup_midas addon ships.
```

## movement

`[movement]` in readyup.cfg.

```ini
// readyup-movement (plugins/movement): surf, bhop and kz with a run timer, personal bests and top
// times. Off while a match is loaded. One key=value per line; comments only on their own lines.
//
// auto = from the map name (surf_, bhop_, kz_ / xc_ / bkz_ / kzpro_); or surf, bhop, kz, off.
// mode=auto
//
// Who sees a finished run in chat: records = the runner, and everyone when it is a new server
// record; self = only the runner; all = everyone.
// finish_chat=records
//
// The map the server goes back to when nobody is on it and the map has no movement mode (a
// restart lands on de_dust2): a map name or a workshop id. Default: 3073875025 (surf_utopia_njv).
// Empty = stay.
// home_map=3073875025
```

## practice

`[practice]` in readyup.cfg.

```ini
// readyup-practice (plugins/practice) settings. Practice mode's cvars are built in and can't be changed
// per server; open an issue if a default should change.
//
// always=1 makes this a dedicated practice server: practice mode is switched on at load and on
// every map (unless a match is loaded). An admin's .prac still turns it off until the next map.
// always=1
//
// feedback=0 turns off the chat lines in practice mode: flash durations, damage per hit, HE and
// fire damage per throw (on by default; everyone sees them).
// feedback=0
//
// Scenarios (.scen, docs/SCENARIOS.md). `ru scenario convert` runs the demo converter:
// scenario_python=python3                  the Python that has demoparser2 (pip install demoparser2)
// scenario_pythonpath=                     extra PYTHONPATH for it (a pip --target folder)
// scenario_converter=                      default: readyup/tools/scenario/ru_scenario.py
```

## tv

`[tv]` in readyup.cfg.

```ini
// readyup-tv (plugins/tv): the server's GOTV settings, kept for a whole series
// (docs/design/tv-plugin.md). Re-read at every map start. One key=value per line; comments only on
// their own lines. Everything is commented out: Ready Up leaves GOTV alone until you set a key.
// `ru tv set <key> <value>` (saved in readyup/plugins/tv/settings.json) wins over this file, a
// platform match's rules.broadcast wins over both for that match.
//
// enabled=1
//
// GOTV delay in seconds (tv_delay, 0..960). Never set below what a match asks for itself
// (ruleset valve: 105).
// delay_seconds=105
//
// Observers with an insecure client (HLAE, e.g. Lexogrine HUD Manager) may watch over GOTV:
// tv_secure_bypass 1 + sv_deltaticks_enforce 0. GOTV only: the game port stays VAC-secured.
// sv_deltaticks_enforce is server-wide, so leave this off unless such an observer needs it.
// allow_insecure_observers=0
//
// tv_maxclients (0..255), tv_relaytextchat, tv_relayvoice.
// max_clients=8
// relay_text_chat=1
// relay_voice=1
//
// Casters: SteamID64s who may join the game port as spectators without being kicked (needs
// whitelist.so outside a match; during a match the match plugin lets them in as spectators).
// casters=76561198000000001, 76561198000000002
//
// How often tv_status is read for the GOTV client count while the platform link is up (seconds;
// 0 = only on `ru tv status`).
// clients_poll_seconds=60
```

## retakes

`[retakes]` in readyup.cfg.

```ini
// readyup-retakes (plugins/retakes): retakes for fun servers. The bomb is planted every round;
// Ts hold the site, CTs retake it. Spawns are CS2's own (maps/<map>_retake.txt), so it runs on the
// maps that have them: de_ancient, de_anubis, de_cache, de_dust2, de_inferno, de_mirage, de_nuke,
// de_overpass, de_train, de_vertigo. Re-read when the plugin loads.
// Every value below is the default; remove the // to change one.
//
// Off until you turn it on: while it is on, retakes runs whenever no match is loaded.
// enabled=0
//
// Players on a team; anyone past this waits in spectator for a free place (2-10).
// max_players=9
//
// The Ts win this many rounds in a row: the teams are shuffled (0 = never).
// scramble_after=5
```
