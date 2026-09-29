# Draft: Ready Up first beta (v0.1.0-beta.1)

> **Draft. Do not merge until the release.** Nothing here has been tagged or published.
> Owner's rule: no Ready Up release until (1) the core/plugin split is done, (2) parity with
> the Auto Tournament CS2 plugin is reached and (3) a real match has been play-tested. See
> "Readiness" at the bottom for where each one stands.
>
> Prepared 2026-09-29 against `master` `36f9d10` (#95–#117). Ships together with Auto
> Tournament 3.0.0-beta.14 and CS2 Server Manager 1.12.0.

---

## Release notes (paste into the GitHub release)

**Ready Up v0.1.0-beta.1: first public beta**

Ready Up is a match server for CS2 that runs without Metamod or CounterStrikeSharp. It is a
small core that loads Ready Up plugins, and a set of plugins on top: the match flow, practice,
admin basics, the link to the Auto Tournament platform, and a few optional extras.

It replaces the old Auto Tournament CS2 plugin. Every feature that plugin had is in Ready Up
([docs/PARITY.md](https://github.com/Auto-Tournament/ready-up/blob/master/docs/PARITY.md):
101 of 101 rows done). It works with the Auto Tournament platform, and on its own without one.

This is the first build we publish. It is a beta: use it for scrims, practice and test
events, and keep your current setup for anything that matters.

### What you get

**Core**
- Loads straight from `gameinfo.gi`. No Metamod, no CounterStrikeSharp, no .NET.
- Every hook it uses is checked against the current CS2 build in CI. If a CS2 update breaks
  something, Ready Up turns off the part that broke and logs why, instead of crashing the
  server. Each plugin says what it needs, and the core skips a plugin whose needs a CS2
  build does not meet. `ru selftest` shows every hook and ends with `PASS` or `FAIL`.
- Plugins can be switched on and off with `ru plugin enable|disable <name>`, and the choice is
  remembered across restarts.
- A local status endpoint (`/status`, `/stream`, `/health`, `/metrics`) bound to 127.0.0.1.
  CS2 Server Manager reads it to show live matches and to avoid restarting a server mid-match.
- Every player `.ru` command is admin-only unless you open it up.
- Reads your Auto Tournament license key and the license answer from the installer, and shows
  their status. It never blocks anything.

**With the Auto Tournament platform** (`ready-up-fleet`)
- One outgoing connection to the platform. Enroll with a one-time code or a fleet key; no
  RCON password, webhook URL or open port to set up.
- The platform assigns matches over it, sees the live state and events, and sends admin
  actions, admin lists, server settings, whitelist, practice and roster changes back.
- **Demos stream to the platform while they record**, resume after a disconnect, and are
  checked with a hash. The local file stays as the copy.
- **Round backups go to the platform** every round, so a match can be restored to any round,
  or moved to another server and resumed there if this one dies.
- Sends the server's public address so players get a working `connect` line.
- Checks Steam for a required CS2 update every 30 minutes and tells the platform. Can be
  drained so it takes no new match.
- Messages wait on disk and are sent again after a restart or a dropped link.

**Without a platform**
- Scrims when the server is idle, `ru match load <url|file>` for a match config, practice
  mode, admins in a small JSON file. Demos are recorded and kept, and uploaded over HTTP only
  if you set an upload URL. Nothing waits for a platform.

**Match** (`ready-up-match`)
- Ready-up with a HUD, `.forceready`, minimum ready per team, auto-ready, spectator ready.
  Substitutes don't count toward the ready total.
- Knife round with `.stay` / `.switch`.
- Tactical and technical pauses with limits, a pause after a restore, forfeit when a team
  leaves.
- Round restore (`.restore`, `.stop` vote), and a match that survives a server crash or
  restart and carries on with its stats.
- Overtime that follows CS2: sides, halftime swap and the end of each overtime block.
- Coaches on CS2's own coach slot, team names and flags, whitelist of the roster.
- Wingman, simulation mode, and an esports mode (`ruleset valve`) with Valve's defaults and
  named overrides.
- GOTV demo per map, with upload.
- Full player stats (kills, damage, utility, KAST, clutches, entry kills, trades ...), also
  on request with `ru_match_stats`.
- `.admin [message]` lets a player call an admin. `.rcon` for the server's own admins, with
  dangerous commands refused.
- Old round backups and demos are cleaned up (`backup_keep_hours=72`, `demo_keep_hours=24`;
  `0` keeps them). Files of the current match are never touched.

**Practice** (`ready-up-practice`)
- `.prac`, `.savepos` / `.loadpos`, `.spawn`, `.bot`, `.noflash`, `.god`, `.impacts`,
  `.traj`, `.solid`, `.break`, `.timer`, `.dryrun`, best and worst spawns, `.showspawns`,
  `.fas` / `.watchme`.
- **Lineups:** `.savenade`, `.loadnade`, `.listnades`, `.deletenade`, `.importnade` (codes
  from the old plugin import), and a global list for admins.
- **Rethrow:** `.rethrow` your own last grenade, `.throwidx` any grenade from your history, or
  rethrow by type.
- **Pro round replay:** `.scen` loads a recorded round from a demo. You play one of the ten
  players, from their position with their health, armor, money and weapons, and bots replay
  the other nine, including their smokes, flashes, HEs and molotovs. A bot takes over with its
  own AI once it sees you. A converter turns your own demos into scenarios. See
  [docs/SCENARIOS.md](https://github.com/Auto-Tournament/ready-up/blob/master/docs/SCENARIOS.md).

**Other plugins**
- `ready-up-essentials-plugin`: admins (`admins.json`), map change / reload / restart, Workshop
  maps with a download progress bar, default maps per mode.
- `ready-up-whitelist`: only listed players may stay.
- `ready-up-deathmatch`: free for all and team deathmatch with kill and time limits.
- `ready-up-addons`: Steam Workshop addons on the server, without Metamod.
- `ready-up-skins`: weapon paints, knives, gloves, agents. **Not in the default bundle.**
  Servers that change skins can be banned by Valve. Only install it on purpose.
- `ready-up-midas`: a fun plugin that turns chosen players' weapons gold. Off by default and
  never active under the valve ruleset.
- `ready-up-hello`: an example for plugin developers.

### Downloads

| File | What is in it |
|---|---|
| `ready-up-essentials-0.1.0-beta.1-linuxsteamrt64.zip` | core + essentials + match + fleet + practice. **The default.** No skins. |
| `ready-up-full-0.1.0-beta.1-linuxsteamrt64.zip` | everything: the above + skins, midas, whitelist, deathmatch, addons, hello, and the offline gamedata checkers |
| `ready-up-core-…`, `-match-…`, `-fleet-…`, `-practice-…`, `-essentials-plugin-…`, `-skins-…`, `-midas-…`, `-whitelist-…`, `-deathmatch-…`, `-addons-…`, `-hello-…` | single components, for the installer or a manual mix |
| `SHA256SUMS` | checksums (the installer checks them) |

### Install

From the server root (the folder that has `game/`), as the server's user:

```bash
curl -fsSL https://raw.githubusercontent.com/Auto-Tournament/ready-up/master/install.sh \
  | bash -s -- --channel beta
```

`--channel beta` is needed while this is a pre-release: without it the installer looks for the
latest *stable* release, finds none, and says so. `--version v0.1.0-beta.1` pins this exact
build instead. Keep passing `--channel beta` on updates.

The installer asks once how you use Ready Up (non-commercial, or commercial with a license
key now or later) and you type `I AGREE`. Unattended installs pass the answer as a flag:

```bash
bash install.sh essentials --channel beta --accept-license=noncommercial
bash install.sh essentials --channel beta --accept-license=commercial --license-key ATL1...
```

With CS2 Server Manager 1.12.0, `csm plugins` can install and update Ready Up for you on
every server, on the beta channel.

After a CS2 update, CS2 rewrites `gameinfo.gi` and drops the Ready Up line. csm 1.12.0 puts it
back; otherwise run the installer again.

### Coming from the old Auto Tournament CS2 plugin

- Don't run both match plugins on one server. Take the old plugin off the server you put
  Ready Up on. Metamod and CounterStrikeSharp can stay if you need them for something else
  (Metamod's line goes first in `gameinfo.gi`).
- Ready Up doesn't use the old RCON + webhook contract. It talks to the platform over the
  fleet link instead (Auto Tournament 3.0.0-beta.14 or newer). A platform keeps driving its
  RCON servers as before, so you can move one server at a time.
- Commands and chat aliases from the old plugin work, including the practice ones.
- There is no database. Ready Up keeps its data in small JSON files.

### Licensing

Free for non-commercial use (PolyForm Noncommercial 1.0.0). For commercial use, every server
running Ready Up needs a license, including spare, practice and test servers. The license
never blocks or turns off anything. See
[autotournament.gg/pricing](https://autotournament.gg/pricing).

### Known limitations

- **Beta.** The platform link has been play-tested with bots (a Bo1 with overtime, streamed
  demo). Matches with human players are still being checked.
- Of the 101 parity rows, 5 are proven on real servers over many matches; the rest are
  covered by tests.
- The server drain state is kept in memory, so a restart clears it.
- `.loadnade` can't turn your view (CS2 doesn't allow it); it prints the `setang` line to
  type.
- Pro round replay: the bomb isn't planted and bots don't crouch, jump or shoot while they
  follow the recording.
- Linux (`linuxsteamrt64`) only.

---

## How this release is cut (for the owner)

- **Version lives in** `VERSION` (plain text, currently `0.1.0`). CMake bakes it into the core
  (`READYUP_SEMVER`) and refuses anything that isn't SemVer; the Build workflow checks that a
  tag equals `v` + `VERSION`.
- **How:** from a clean `master`, `./release.sh --dry-run 0.1.0-beta.1`, then
  `./release.sh 0.1.0-beta.1`. It writes `VERSION`, commits "Release v0.1.0-beta.1", tags and
  pushes. The tag runs `.github/workflows/build.yml`: build + sigcheck/hookcheck against the
  current CS2 build, package, installer tests (including the beta channel), then
  `gh release create --prerelease` (never `--latest` for a suffixed tag), then Discord.
  Full steps: `docs/RELEASING.md` (#99).
- **Permissions/secrets:** `github.token` with `contents: write`. Optional
  `DISCORD_WEBHOOK_URL` (skipped when unset).
- **Notes:** `scripts/ci/release-notes.sh` builds the download table from the zips and adds a
  pre-release callout. Replace the text above it with this file afterwards:
  `gh release edit v0.1.0-beta.1 --notes-file <the part above the line>`, keeping the generated
  download table if you prefer it.
- **License line date:** a tag `vX.Y.Z` takes the date of tag `vX.Y.0`; the first `x.y.0`
  (and its betas) use the build day.
- **Order:** this goes first, before csm 1.12.0 and platform 3.0.0-beta.14, so csm's beta
  channel and the platform's update check can find it.

---

## Readiness

| Check | Status |
|---|---|
| CI on `master` | Green: Build passed on `36f9d10`, `61e4e19`, `49998d1`. |
| Pre-release pipeline | **Done** (#99): `release.sh` takes `-beta.N`, `build.yml` publishes suffixed tags as pre-releases, the notes table is generated from the zips, `install.sh --channel beta`. |
| Docs | Draft Ready Up section is `Auto-Tournament/docs#8` (draft, "publish with first release"). Its install page should say `--channel beta` while only pre-releases exist. |
| Migrations | None. First release, so no upgrade path from an earlier Ready Up release. |
| Upgrade path | From the old AT CS2 plugin: per server, see above. The platform keeps RCON servers working next to fleet servers (`cs2_servers.transport` defaults to `rcon`). |
| **Owner condition 1: plugin split** | **Done.** Core + 11 components as separate `.so` plugins, hot enable/disable, bundles checked in CI (`check-bundles.sh`). |
| **Owner condition 2: parity** | **Done on paper.** `docs/PARITY.md` (audited 2026-09-29): 101 of 101 rows done, 0 partial, 0 missing. Only 5 rows are "stable" (proven over many matches); the rest are "tested" or, for drain, "untested". |
| **Owner condition 3: real play-test** | **Partly.** M1 play-test on readyup-test with the platform's fleet driver and bots: Bo1 with MR8 + overtime, demo streamed. It found #111 (GOTV not recording), #112/#113 (overtime), #116 (demo left running), #117 (subs in the ready total); all merged. **Still to do:** a Bo3 with a pause and a restore, and a human match with Sivert. |
| Blockers | Condition 3 (the human match) is the owner's call. Nothing else in this repo. |
| Wording to fix separately | README and `docs/INSTALL.md` say commercial use "needs a paid license"; the rule is "you need a license". |

## Compatibility (betas)

| | Ready Up 0.1.0-beta.1 |
|---|---|
| Fleet protocol | v1 only (`plugins/fleet/fleet_proto.h`: `kProtocolVersion = 1`, hello sends `min 1, max 1`) |
| Plugin API | ru_api 1.13 (`core/include/readyup/plugin_api.h`) |
| Auto Tournament platform | **≥ 3.0.0-beta.14** for the fleet link, match assignment, demo streaming, round backups and failover. 3.0.0-beta.13 and older have no fleet gateway. The version check parses `0.1.0-beta.1 (<sha>)` from beta.14 (#425 there). |
| CS2 Server Manager | **1.12.0**: installs and updates Ready Up (stack `readyup`, channel `beta`), restores the `gameinfo.gi` line after CS2 updates, host agent, instance mode. 1.11.0: license key hand-off only; install Ready Up with `install.sh`. |
| Auto Tournament CS2 plugin | Never on the same server as Ready Up's match plugin. |
| CS2 | The build CI verified is printed in the release notes. |
