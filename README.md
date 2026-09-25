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
- Practice mode: `.prac`, `.bot`, `.cbot`, `.nobots`
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

In a terminal it shows the components with the installed and latest version. Move with ↑/↓, toggle with space, confirm with enter:

```
> [x] Core   new 0.2.0     required
  [x] Match  new 0.2.0     ready-up, knife, pauses, webhooks
  [x] Fleet  new 0.2.0     link to the Auto Tournament platform (idle until configured)
  [ ] Skins  new 0.2.0     may get servers banned
  [ ] Hello  new 0.2.0     example plugin
```

It downloads the ticked components from the latest release (checking `SHA256SUMS`), puts them in `game/csgo/readyup/`, and adds `Game csgo/readyup` to `gameinfo.gi` and `gameinfo_branchspecific.gi` (right after Metamod's line if you have Metamod; a backup is saved as `gameinfo.gi.readyup-backup-<time>`). Your `readyup.cfg`, `cfg/ReadyUp/*.cfg` and the plugins' JSON data are never overwritten: when a shipped default changes, it lands next to yours as `*.default`. Then restart the server and run `ru selftest` in its console.

**Update or change components:** run the same command again. Unticking an installed component removes it.

**Scripts and panels (no questions):**

```bash
curl -fsSL .../install.sh | bash -s -- essentials        # core + match + fleet (the default)
curl -fsSL .../install.sh | bash -s -- full              # + skins + hello
bash install.sh --yes                                    # update whatever is installed
bash install.sh --remove skins                           # or --remove fleet, --remove hello
bash install.sh --uninstall [--purge]                    # --purge also deletes your config
bash install.sh --zip ready-up-essentials-<v>-linuxsteamrt64.zip essentials   # offline / CI artifact
```

Other options: `--dir /path/to/cs2`, `--version vX.Y.Z`. It needs bash, python3, curl or wget, and unzip (python3 is used if unzip is missing). It never uses sudo, never stops or starts the server, and never touches your data files.

CS2 updates rewrite `gameinfo.gi`: run the installer again after each one (it only re-adds the line).

### Manual install

1. Download a zip from [Releases](https://github.com/Auto-Tournament/ready-up/releases/latest): `ready-up-essentials-<version>-linuxsteamrt64.zip` (core + match + fleet) or `ready-up-full-...` (+ skins + hello).
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

Everything lives in this repo. The core is always installed; plugins are separate `.so` files you add or leave out, and they hot reload without restarting the server (`ru plugin reload <name>`).

<details>
<summary><b>Core</b> (<code>core/</code>): loader, engine layer, plugin host</summary>

<br />

Loads into CS2, owns every engine touchpoint (`gamedata/engine-surface.json`), and gives plugins a small, versioned C API: chat and console commands, game and log events, center-screen HTML, server commands, player and team lookup. Also ships `ru selftest` and the local status endpoint. It runs on its own too: without plugins it checks the engine and serves `/status`, but there is no match flow.

</details>

<details>
<summary><b>Match</b> (<code>plugins/match</code>): ready-up, knife, pauses, practice</summary>

<br />

The match flow: scrim ready-up with a center-screen panel, knife round and side pick, pauses, practice mode, admins, match configs, webhooks for the Auto Tournament platform, GOTV demos and per-map stats. Ships as `plugins/match.so` in both bundles. `ru plugin reload match` swaps in a new build without dropping a loaded match: ready states, scores and the knife round carry over.

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
<summary><b>Hello</b> (<code>plugins/hello</code>): example plugin</summary>

<br />

A minimal plugin that registers `.hello` in chat. Start here to write your own.

</details>

Downloads: `ready-up-core`, `ready-up-match`, `ready-up-fleet`, `ready-up-skins`, `ready-up-hello`, and two bundles: **Essentials** (core + match + fleet) and **Full** (core + match + fleet + skins + hello + the gamedata checkers). The installer mixes the single components. `fleet` is the link to the Auto Tournament platform; it stays idle until you set a `url` in `cfg/ReadyUp/fleet.cfg` (shipped fully commented out), so it is safe on standalone servers.

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

Looking for an MIT plugin instead? [MatchZy Enhanced](https://github.com/Auto-Tournament/cs2-plugin) (now named Auto Tournament CS2) is MIT licensed and free for any use, including paid work. Ready Up is a different plugin, not a fork of it.

Third-party code under `third_party/` keeps its own license.
