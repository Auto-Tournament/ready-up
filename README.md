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
- Admins stored in Postgres
- Per-player center-screen HTML, such as the welcome screen
- Webhooks and a heartbeat for the platform
- Knife round (in progress)

Skins (weapon paints, knives, gloves, agents) are a separate plugin and aren't in the default release. Skin changers can get a server banned, so you have to download it on purpose.

## Install

Ready Up runs on Linux dedicated servers (`linuxsteamrt64`).

1. Download `readyup-<version>-linuxsteamrt64.zip` from [Releases](https://github.com/Auto-Tournament/ready-up/releases/latest).
2. Extract it into `game/csgo`. You should end up with `game/csgo/readyup/bin/linuxsteamrt64/libserver.so`.
3. Add Ready Up to `gameinfo.gi`:

   ```bash
   cd game/csgo
   python3 readyup/tools/patch_gameinfo.py gameinfo.gi --game csgo/readyup
   ```

   If you have `gameinfo_branchspecific.gi`, run it on that file too.
4. Restart the server.

The patcher adds `Game csgo/readyup` to `SearchPaths`. It has to be listed **before** `Game csgo`, or CS2 loads its own `libserver.so` and Ready Up never runs. You can check the file by hand afterwards.

Metamod can run alongside Ready Up. Keep its `Game csgo/addons/metamod` line above Ready Up's.

CS2 updates rewrite `gameinfo.gi`, so run the patcher again after each one.

Config, admins and the database are covered in [docs/INSTALL.md](docs/INSTALL.md) and [docs/ADMINS.md](docs/ADMINS.md).

## Plugins

Everything lives in this repo. The core is always installed; plugins are separate `.so` files you add or leave out, and they hot reload without restarting the server (`ru plugin reload <name>`).

<details>
<summary><b>Core</b> (<code>core/</code>): loader, engine layer, plugin host</summary>

<br />

Loads into CS2, owns every engine touchpoint (`gamedata/engine-surface.json`), and gives plugins a small, versioned C API: chat and console commands, game and log events, center-screen HTML, server commands, player and team lookup. Also ships `ru selftest`.

</details>

<details>
<summary><b>Match</b> (<code>plugins/match</code>): ready-up, knife, pauses, practice</summary>

<br />

The match flow: scrim ready-up with a center-screen panel, knife round and side pick, pauses, practice mode, admins, match configs and webhooks for the Auto Tournament platform. Ships as `plugins/skins.so` plus its gamedata `engine-surface.skins.json`; the core runs without either.

</details>

<details>
<summary><b>Skins</b> (<code>plugins/skins</code>): optional, not in the default bundle</summary>

<br />

Weapon paints, knives, gloves and agents from a Postgres table. Skin changers can get a server banned, so this plugin is only in the Full bundle and you add it on purpose. Currently built into the core while it moves to its own plugin.

</details>

<details>
<summary><b>Hello</b> (<code>plugins/hello</code>): example plugin</summary>

<br />

A minimal plugin that registers `.hello` in chat. Start here to write your own.

</details>

Planned downloads: `ready-up-core`, `ready-up-match`, `ready-up-skins`, and two bundles: **Essentials** (core + match) and **Full** (core + match + skins).

## Documentation

Full docs are at **[docs.autotournament.gg](https://docs.autotournament.gg)**. In this repo:

- [Install and how loading works](docs/INSTALL.md)
- [Admins and database](docs/ADMINS.md)
- [Development and debugging](docs/DEVELOPMENT.md)
- [Testing with Auto Tournament](docs/TESTING_WITH_MAT.md)

## Contributing

See the [contributing guide](.github/CONTRIBUTING.md). Questions and bug reports are welcome on [Discord](https://discord.gg/n7gHYau7aW).

## License

Ready Up is licensed under the [PolyForm Noncommercial License 1.0.0](LICENSE). Copyright (c) 2026 Sivert Gullberg Hansen.

You can use, change and share it for anything noncommercial. Commercial use, like paid events or selling it or a service built on it, needs a separate license. Ask on [Discord](https://discord.gg/n7gHYau7aW).

Third-party code under `third_party/` keeps its own license.
