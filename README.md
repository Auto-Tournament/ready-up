<div align="center">
  <img src=".github/logo.svg" width="100" alt="Ready Up logo" />
  <h1>Ready Up</h1>
  <p><strong>A native CS2 server plugin for scrims, pickups and tournament matches</strong></p>
  <p>
    <a href="https://github.com/Auto-Tournament/ready-up/releases/latest"><img src="https://img.shields.io/github/v/release/Auto-Tournament/ready-up?cacheSeconds=3600" alt="GitHub Release" /></a>
    <a href="LICENSE"><img src="https://img.shields.io/badge/License-PolyForm%20Noncommercial-blue.svg" alt="License: PolyForm Noncommercial 1.0.0" /></a>
    <a href="https://docs.autotournament.gg"><img src="https://img.shields.io/badge/docs-docs.autotournament.gg-blue" alt="Docs" /></a>
    <a href="https://discord.gg/n7gHYau7aW"><img src="https://img.shields.io/badge/Discord-join-5865F2?logo=discord&logoColor=white" alt="Discord" /></a>
  </p>
</div>

<br />

> [!CAUTION]
> **Early development.** Ready Up changes often. Expect breaking changes between releases.

Ready Up runs your CS2 server's match flow: players ready up, the match goes live, and results go back to whatever is running the event. It's part of [Auto Tournament](https://github.com/Auto-Tournament/auto-tournament) and does the same job as [MatchZy](https://github.com/shobhit-pathak/MatchZy) and [Get5](https://github.com/splewis/get5). It isn't a fork of either. We borrow their commands and event names where it makes sense, so moving over is easy.

**No Metamod. No CounterStrikeSharp. Nothing else to install.**

Ready Up is its own small ecosystem: think of it as Metamod and its plugins in one bundle. A small core loads straight into CS2 through a `gameinfo.gi` entry, the same way Metamod does, then loads Valve's real server module and passes everything through. Plugins run on top of the core and never touch the engine themselves, so a CS2 update only ever means fixing the core.

## Why it keeps working after CS2 updates

Ready Up touches the engine in as few places as it can. Every function it calls or hooks is listed in [`gamedata/engine-surface.json`](gamedata/engine-surface.json), and each one has a signature plus identity anchors: strings the function or its callers must reference. A function only resolves if the signature matches exactly once and every anchor checks out. If one breaks, just that feature turns itself off. The rest of the server keeps running.

CI checks the file against every new CS2 build, so we see what broke before a server does. On a running server, `ru selftest` shows every hook, offset and feature, and ends with `PASS` or `FAIL`.

## Features

- Ready-up flow for scrims and pickups: `.r` / `.ur`, a countdown, then the match goes live on its own
- Match configs loaded from the Auto Tournament platform, with a roster whitelist and team locks
- Pauses (`.pause` / `.unpause`) and captain forfeit
- Practice mode (its own plugin): `.prac`, `.savepos`/`.loadpos`, `.spawn N`, `.rethrow`, `.bot`, `.noflash`, `.god`; a dedicated practice server with `always=1`
- Admins, settings, crash recovery and skins loadouts in small JSON files, no database (`ru admins add|remove|list`); in fleet mode admins and loadouts come from the platform
- Per-player center-screen HTML, such as the welcome screen
- Webhooks and a heartbeat for the platform
- Knife round (in progress)

Skins (weapon paints, knives, gloves, agents) are a separate plugin and aren't in the default release. Skin changers can get a server banned, so you have to download it on purpose.

## Install

Ready Up runs on Linux dedicated servers (`linuxsteamrt64`). From your server root (the folder that contains `game/`), as the user that owns the server files:

```bash
curl -fsSL https://raw.githubusercontent.com/Auto-Tournament/ready-up/master/install.sh | bash
```

It first asks how you will use Ready Up: personal / noncommercial (you type `yes` to accept the
[license](#license)) or commercial (you need a paid license first, see [Commercial use](#commercial-use)).
The choice is saved in `game/csgo/readyup/license-acceptance.json`, so updates don't ask again.

In a terminal it then shows the components with the installed and latest version. Move with ↑/↓, toggle with space, confirm with enter:

```
> [x] Core   new 0.2.0     required
  [x] Match  new 0.2.0     ready-up, knife, pauses, webhooks
  [x] Fleet  new 0.2.0     link to the Auto Tournament platform (idle until configured)
  [ ] Skins  new 0.2.0     may get servers banned
  [ ] Hello  new 0.2.0     example plugin
  [ ] Midas  new 0.2.0     fun: gold weapons (off until enabled)
  [ ] Whitelist new 0.2.0  only listed players may join (off until turned on)
  [x] Practice new 0.2.0   practice mode + tools (.prac, .savepos, .rethrow, .bot)
  [x] Essentials new 0.2.0 admins + map commands (needed for admins without a match config)
```

It downloads the ticked components from the latest release (checking `SHA256SUMS`), puts them in `game/csgo/readyup/`, and adds `Game csgo/readyup` to `gameinfo.gi` and `gameinfo_branchspecific.gi` (right after Metamod's line if you have Metamod; a backup is saved as `gameinfo.gi.readyup-backup-<time>`). Your `readyup.cfg`, `cfg/ReadyUp/*.cfg` and the plugins' JSON data are never overwritten: when a shipped default changes, it lands next to yours as `*.default`. Then restart the server and run `ru selftest` in its console.

**Update or change components:** run the same command again. Unticking an installed component removes it.

**Scripts and panels (no questions):**

Unattended installs have to state the license choice once with
`--accept-license=noncommercial` or `--accept-license=commercial`, or they stop with an error:

```bash
curl -fsSL .../install.sh | bash -s -- essentials --accept-license=noncommercial   # core + essentials + match + fleet + practice (the default)
curl -fsSL .../install.sh | bash -s -- full --accept-license=noncommercial         # + skins + hello + midas
bash install.sh --yes                                    # update whatever is installed
bash install.sh --remove skins                           # or --remove fleet, --remove hello
bash install.sh --uninstall [--purge]                    # --purge also deletes your config
bash install.sh --zip ready-up-essentials-<v>-linuxsteamrt64.zip essentials   # offline / CI artifact
```

Other options: `--dir /path/to/cs2`, `--version vX.Y.Z`. It needs bash, python3, curl or wget, and unzip (python3 is used if unzip is missing). It never uses sudo, never stops or starts the server, and never touches your data files.

CS2 updates rewrite `gameinfo.gi`: run the installer again after each one (it only re-adds the line).

### Manual install

1. Download a zip from [Releases](https://github.com/Auto-Tournament/ready-up/releases/latest): `ready-up-essentials-<version>-linuxsteamrt64.zip` (core + essentials + match + fleet + practice) or `ready-up-full-...` (+ skins + hello + midas).
2. Extract it into `game/csgo`. You should end up with `game/csgo/readyup/bin/linuxsteamrt64/libserver.so`.
3. Add Ready Up to `gameinfo.gi` (and `gameinfo_branchspecific.gi` if you have it):

   ```bash
   cd game/csgo
   python3 readyup/tools/patch_gameinfo.py gameinfo.gi --game csgo/readyup
   ```
4. Restart the server.

The patcher adds `Game csgo/readyup` to `SearchPaths`. It has to be listed **before** `Game csgo`, or CS2 loads its own `libserver.so` and Ready Up never runs. With Metamod, Metamod's `Game csgo/addons/metamod` line stays above Ready Up's.

Config, admins and the data files are covered in [docs/INSTALL.md](docs/INSTALL.md) and [docs/ADMINS.md](docs/ADMINS.md).

## Plugins

Ready Up is a core plus plugins, like Metamod and its plugins. The core (`libserver.so`) is the
only part that touches the engine; each feature is its own plugin (`csgo/readyup/plugins/<name>.so`)
that talks to the core through the versioned C API in
[`core/include/readyup/plugin_api.h`](core/include/readyup/plugin_api.h). Every plugin ships as its own
zip and can be installed, hot reloaded, or turned off (`ru plugin disable <name>`) on its own; the
bundles below are just zips with several of them. The in-house plugins use nothing but that public
API, so they double as examples for your own.

Plugins cooperate through named interfaces (`provide_interface` / `get_interface`): the match plugin
publishes `readyup.match.v1` (mode, ruleset, map stats), the practice plugin `readyup.practice.v1`, the whitelist
plugin `readyup.whitelist.v1`, the skins plugin `readyup.skins.v1` (paint one weapon; Midas uses it). The match plugin owns the match phase; the others read it and stand
down while a match is loaded or live, and under the valve ruleset. None of them needs another to be
loaded: each checks for the interface and works on its own (a practice-only server is core + practice).

Everything lives in this repo. The core is always installed; plugins are separate `.so` files you add or leave out, and they hot reload without restarting the server (`ru plugin reload <name>`). `ru plugin list` shows them; `ru plugin disable <name>` / `enable <name>` turns one off or on and keeps it that way after a restart.

<details>
<summary><b>Core</b> (<code>core/</code>): loader, engine layer, plugin host</summary>

<br />

Loads into CS2, owns every engine touchpoint (`gamedata/engine-surface.json`), and gives plugins a small, versioned C API: chat and console commands, game and log events, center-screen HTML, server commands, player and team lookup. Also ships `ru selftest` and the local status endpoint. It runs on its own too: without plugins it checks the engine and serves `/status`, but there is no match flow.

</details>

<details>
<summary><b>Match</b> (<code>plugins/match</code>): ready-up, knife, pauses</summary>

<br />

The match flow: scrim ready-up with a center-screen panel, knife round and side pick, pauses, admins, match configs, webhooks for the Auto Tournament platform, GOTV demos and per-map stats. Ships as `plugins/match.so` in both bundles. `ru plugin reload match` swaps in a new build without dropping a loaded match: ready states, scores and the knife round carry over.

</details>

<details>
<summary><b>Essentials</b> (<code>plugins/essentials</code>): admins and map commands</summary>

<br />

Server basics kept apart from the match flow, so every kind of server has them: the admins list (`ru admins`, `plugins/essentials/admins.json`) and `.ru map change <name|workshop id|link>` / `reload` / `restart` (refused during a live map unless `force`). While the server downloads a Workshop map, everyone sees a progress bar in the center of the screen. A practice-only server is core + essentials + practice; an esports server can run core + match alone. In both bundles.

</details>

<details>
<summary><b>Practice</b> (<code>plugins/practice</code>): practice mode and tools</summary>

<br />

`.prac` (admin) switches practice on or off: `cfg/ReadyUp/prac.cfg` (cheats, a full grenade set, infinite ammo) and everyone respawns with it. Tools: `.savepos`/`.loadpos [name]`, `.back`, `.spawn N` / `.ctspawn N` / `.tspawn N`, `.rethrow`, `.clear`, `.noflash`, `.god`, `.bot`/`.cbot`/`.boost`, `.nobots`. Runs with the match plugin (which then shows practice as its mode and refuses it while a match is loaded) or without it: `always=1` in `cfg/ReadyUp/practice.cfg` makes a dedicated practice server. Never active under the valve ruleset. In both bundles.

</details>

<details>
<summary><b>Fleet</b> (<code>plugins/fleet</code>): link to the Auto Tournament platform</summary>

<br />

One outbound WebSocket to the platform: enrollment, match assignment and commands, state stream, offline spool ([FLEET.md](docs/FLEET.md)). Ships as `plugins/fleet.so` in both bundles plus a fully commented `cfg/ReadyUp/fleet.cfg`. Without a `url` it loads, logs one line and stays idle, so standalone servers are unaffected.

</details>

<details>
<summary><b>Skins</b> (<code>plugins/skins</code>): optional, not in the default bundle</summary>

<br />

Weapon paints, knives, gloves and agents from `loadouts.json` ([contract](plugins/skins/docs/json-contract.md)), or from the platform in fleet mode. Skin changers can get a server banned, so this plugin is only in the Full bundle and you add it on purpose. Ships as `plugins/skins.so` plus its gamedata `engine-surface.skins.json`; the core runs without either.

</details>

<details>
<summary><b>Midas</b> (<code>plugins/midas</code>): fun, Full bundle only</summary>

<br />

Weapons picked up by Midas players turn gold, and stay gold when someone else picks them up. Off by default (`cfg/ReadyUp/midas.cfg`, `enabled=1`) and never active under the valve ruleset. Hot reloads with `ru plugin reload midas`.

- **Who**: the players in `midas_steamids`, and with `best_player=1` the best player of the map: top ADR or kills (`best_player_stat=adr|kills`) from the match plugin's stats, picked a few ticks after each round start once `best_player_min_rounds` (3) rounds are played, or at the start of every new half (`best_player_when=half`). Scrims only; real matches need `best_player_in_matches=1`. Ties go to the other stat, then fewer deaths, then the current Midas.
- **Gold**: with the skins plugin loaded, a gold paint kit through its `readyup.skins.v1` interface (`paint_kit=1025`, "Gold Brick", a pattern finish that fits every gun; `paint_wear`, `paint_seed`; any paint kit id works). Knives, grenades and the C4, and every weapon without skins.so or with `finish=tint`, get the render colour `color=255,200,40` instead (a server can't send custom textures).

</details>

<details>
<summary><b>Whitelist</b> (<code>plugins/whitelist</code>): only listed players, Full bundle</summary>

<br />

For practice and scrim servers: `ru whitelist on`, `ru whitelist add <steamid64>`, and anyone else who joins is kicked (admins and bots stay). Stands down while a match is loaded (the match roster decides then). The list is saved in `plugins/whitelist/whitelist.json`.

</details>

<details>
<summary><b>Hello</b> (<code>plugins/hello</code>): example plugin</summary>

<br />

A minimal plugin that registers `.hello` in chat. Start here to write your own.

</details>

Downloads: `ready-up-core`, `ready-up-match`, `ready-up-fleet`, `ready-up-skins`, `ready-up-hello`, `ready-up-midas`, `ready-up-whitelist`, `ready-up-practice`, `ready-up-essentials-plugin`, and two bundles: **Essentials** (core + essentials + match + fleet + practice) and **Full** (core + essentials + match + fleet + practice + skins + hello + midas + whitelist + the gamedata checkers). The installer mixes the single components. `fleet` is the link to the Auto Tournament platform; it stays idle until you set a `url` in `cfg/ReadyUp/fleet.cfg` (shipped fully commented out), so it is safe on standalone servers.

## FAQ

Run Ready Up on its own. The answers below were tested on a live server; the full results
are in [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md).

**Can I run it with Metamod?** Yes. Metamod's line goes first in `gameinfo.gi` and Ready Up's
directly below it (the installer does this). If Ready Up ends up above Metamod, Metamod
doesn't load and Ready Up logs a warning with the fix.

**With CounterStrikeSharp?** It loads and nothing crashes, but we don't recommend it:
CSSharp's `Host_Say` chat hook is lost next to Ready Up, and when a CS2 update breaks
CSSharp's gamedata, CSSharp crashes the whole server. Ready Up can't stop that.

**With MatchZy, Get5 or the Auto Tournament CS2 plugin?** No. Never run two match plugins:
both handle `.r`, they overwrite each other's cvars and every match gets reported twice. Use
Ready Up's match plugin. It has the same commands.

**What if I need a CounterStrikeSharp plugin?** Port it to the Ready Up plugin API. Start
with [`plugins/hello`](plugins/hello), the plugin API in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and
[`core/include/readyup/plugin_api.h`](core/include/readyup/plugin_api.h).

## Documentation

Full docs are at **[docs.autotournament.gg](https://docs.autotournament.gg)**. In this repo:

- [Install and how loading works](docs/INSTALL.md)
- [Running next to Metamod / CounterStrikeSharp](docs/COMPATIBILITY.md)
- [Admins](docs/ADMINS.md)
- [Esports mode (Valve ruleset) spec](docs/ESPORTS-MODE.md)
- [Development and debugging](docs/DEVELOPMENT.md)
- [Testing with Auto Tournament](docs/TESTING_WITH_MAT.md)

## Contributing

See the [contributing guide](.github/CONTRIBUTING.md). Questions and bug reports are welcome on [Discord](https://discord.gg/n7gHYau7aW).

## License

Ready Up is licensed under the [PolyForm Noncommercial License 1.0.0](LICENSE). Copyright (c) 2026 Sivert Gullberg Hansen. Ready Up is not MIT.

You can use, change and share it for anything noncommercial. Commercial use, meaning anyone who earns money from it (a profit-making event, a business, a paid operator, or selling it or a service built on it), needs a separate license — see [pricing](https://autotournament.gg/pricing) or email [sivert@autotournament.gg](mailto:sivert@autotournament.gg).

### Commercial use

The free license covers noncommercial use only. If you or your organization earn money from
Ready Up (a business, a profit-making event or tournament, a paid server operator, or selling
Ready Up or a service built on it), you need a paid commercial license before you install it.
See [pricing](https://autotournament.gg/pricing) or email
[sivert@autotournament.gg](mailto:sivert@autotournament.gg). Then install with
`--accept-license=commercial`. The server prints a one-line license notice at every start.

Looking for an MIT plugin instead? [MatchZy Enhanced](https://github.com/Auto-Tournament/cs2-plugin) (now named Auto Tournament CS2) is MIT licensed and free for any use, including paid work. Ready Up is a different plugin, not a fork of it.

Third-party code under `third_party/` keeps its own license.
