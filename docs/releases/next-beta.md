# Draft: Ready Up first beta (v0.1.0-beta.1)

> **Draft. Do not merge until the release.** Nothing here has been tagged or published.
> Owner's rule: no Ready Up release until (1) the core/plugin split is done, (2) parity with
> the Auto Tournament CS2 plugin is reached and (3) a real match has been play-tested. See
> "Readiness" at the bottom for where each one stands.
>
> Prepared 2026-09-29 against `master` `4e42de0`.

---

## Release notes (paste into the GitHub release)

**Ready Up v0.1.0-beta.1 — first public beta**

Ready Up is a match server for CS2 that runs without Metamod or CounterStrikeSharp. It is a
small core that loads Ready Up plugins, and a set of plugins on top: the match flow, practice,
admin basics, the link to the Auto Tournament platform, and a few optional extras.

This is the first build we publish. It is a beta: use it for scrims, practice and test
events, and keep your current setup for anything that matters.

### What you get

**Core**
- Loads straight from `gameinfo.gi`. No Metamod, no CounterStrikeSharp, no .NET.
- Every hook it uses is checked against the current CS2 build in CI. If a CS2 update breaks
  something, Ready Up turns off the part that broke and logs why, instead of crashing the
  server. Each plugin says what it needs, and the core skips a plugin whose needs a CS2
  build does not meet.
- Plugins can be switched on and off with `ru plugin enable|disable <name>`, and the choice is
  remembered across restarts.
- A local status endpoint (`/status`, `/stream`, `/health`, `/metrics`) bound to 127.0.0.1.
  CS2 Server Manager reads it to show live matches and to avoid restarting a server mid-match.
- Every player `.ru` command is admin-only unless you open it up.
- Reads your Auto Tournament license key and shows its status. It never blocks anything.

**Match** (`ready-up-match`)
- Ready-up with a HUD, `.forceready`, minimum ready per team, auto-ready, spectator ready.
- Knife round with `.stay` / `.switch`, scrims when the server is idle.
- Tactical and technical pauses with limits, a pause after a restore, forfeit when a team
  leaves.
- Round restore (`.restore`, `.stop` vote), and a match that survives a server crash or
  restart and carries on with its stats.
- Coaches on CS2's own coach slot, team names and flags, whitelist of the roster.
- Wingman, simulation mode, and an esports mode (`ruleset valve`) with Valve's defaults and
  named overrides.
- Demo recording per map, with upload.
- Full player stats (kills, damage, utility, KAST, clutches, entry kills, trades ...).
- `.admin [message]` lets a player call an admin.

**Other plugins**
- `ready-up-practice`: `.prac`, `.savepos` / `.loadpos`, `.spawn`, `.rethrow`, `.bot`,
  `.noflash`, grenade and damage feedback in chat.
- `ready-up-essentials-plugin`: admins (`admins.json`), map change / reload / restart, Workshop
  maps with a download progress bar, default maps per mode.
- `ready-up-fleet`: the link to the Auto Tournament platform (see "Known limitations").
- `ready-up-whitelist`: only listed players may stay.
- `ready-up-deathmatch`: free for all and team deathmatch with kill and time limits.
- `ready-up-addons`: Steam Workshop addons on the server, without Metamod.
- `ready-up-skins`: weapon paints, knives, gloves, agents. **Not in the default bundle.**
  Servers that run skin changers risk a GSLT ban. Only install it on purpose.
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

From the server root (the folder that has `game/`):

```bash
curl -fsSL https://raw.githubusercontent.com/Auto-Tournament/ready-up/master/install.sh \
  | bash -s -- --version v0.1.0-beta.1 essentials
```

`--version` is needed while this is a pre-release: without it the installer looks for the
latest *stable* release and finds none. Unattended installs also need
`--accept-license=noncommercial` or `--accept-license=commercial`.

After every CS2 update, run the installer again (or `python3 readyup/tools/patch_gameinfo.py`):
CS2 updates rewrite `gameinfo.gi` and drop the Ready Up line.

### Coming from the Auto Tournament CS2 plugin (MatchZy fork)

- Don't run both match plugins on one server. Take the AT CS2 plugin off the server you put
  Ready Up on. Metamod and CounterStrikeSharp can stay if you need them for something else
  (Metamod's line goes first in `gameinfo.gi`).
- Ready Up does not speak the old RCON + webhook contract (`matchzy_*` / `at_*` commands).
  It talks to the platform over the fleet link instead. A platform keeps driving its RCON
  servers as before, so you can move one server at a time.
- Postgres is gone. Ready Up keeps its data in JSON files. `readyup/tools/migrate-postgres-to-json.py`
  moves an older Ready Up dev install's data over.

### Known limitations

- **The platform link is new.** Enrollment, the live connection, status, events and the
  license key work. The platform does not yet assign matches to Ready Up servers in any
  released platform version; that arrives with the platform's fleet driver (after
  3.0.0-beta.14 unless it lands first). Until then, load matches locally
  (`ru match load <url>` or a JSON file).
- **Demo upload over the fleet link is not finished.** Recording works and a local upload URL
  works. Streaming demos to the platform over the link is being built.
- **The platform's update check shows "unknown"** for Ready Up servers, because Ready Up
  reports its version as `0.1.0-beta.1 (<commit>)` and the platform does not parse that yet.
- **CS2 Server Manager does not install Ready Up.** Use `install.sh`. csm 1.10+ shows Ready
  Up's live state and refuses to stop or update a server during a match; csm does not put the
  Ready Up line back into `gameinfo.gi` after a CS2 update, so re-run the installer.
- Practice extras from the old plugin are missing: `.last`, `.throw*`, the lineup library
  (`.savenade` ...), `.bestspawn`, `.showspawns`, `.impacts`, `.dry`, `.timer`.
- `.rcon` is not there; the platform uses its own `exec` command instead.
- A few platform settings are accepted but not applied yet: chat prefixes, warmup, demo,
  offline pause minutes and the status port from `server.config`, and `scrim_when_idle`.
- Linux (`linuxsteamrt64`) only.

---

## How this release is cut (for the owner)

- **Version lives in** `VERSION` (plain text, currently `0.1.0`). CMake bakes it into the core
  (`READYUP_SEMVER`); the Build workflow checks that a tag equals `VERSION`.
- **Trigger:** pushing a tag `v*` runs `.github/workflows/build.yml`. Jobs: `build` (sniper
  SDK, unit + ctest, sigcheck/hookcheck against the current CS2 build, `scripts/package-release.sh`,
  `scripts/ci/check-bundles.sh`), `installer` (shellcheck + `tests/installer/test_install.sh`),
  then `release` (only on tags): `gh release create <tag> … --latest`, then Discord.
- **Local helper:** `./release.sh [major|minor|patch|X.Y.Z]` bumps `VERSION`, commits, tags and
  pushes, then watches the run.
- **Permissions/secrets:** the `release` job uses `github.token` with `contents: write`.
  Optional secret `DISCORD_WEBHOOK_URL` (skipped when unset). No other secrets.
- **License line date:** a tag `vX.Y.Z` takes the date of tag `vX.Y.0`; the first `x.y.0`
  (and its betas) use the build day.

### What has to change before a *beta* can be cut

The pipeline only knows stable versions today:

1. `release.sh` rejects `0.1.0-beta.1` (`validate_semver` accepts only `X.Y.Z`). Either tag
   by hand (`printf '0.1.0-beta.1\n' > VERSION`, commit, `git tag v0.1.0-beta.1`, push), or
   let `release.sh` accept `-beta.N`.
2. `build.yml`'s `release` job always passes `--latest`. For a beta it should pass
   `--prerelease` instead (for example when the tag contains `-`).
3. `scripts/ci/release-notes.sh` has an out-of-date download table: it says "full" is
   "core + match + skins + hello + gamedata checkers" and lists 4 single components. There
   are 11 now. Either fix the table or replace the generated notes with this file after the
   run (`gh release edit v0.1.0-beta.1 --notes-file docs/releases/next-beta.md`, using the
   part above the line).
4. A pre-release is invisible to `releases/latest`, which both `install.sh` (default) and the
   platform's version check read. That is why the notes above say `--version`. Publishing
   `v0.1.0` as a normal release instead avoids this, at the cost of not saying "beta" in the
   tag.

---

## Readiness

| Check | Status |
|---|---|
| CI on `master` | Green. Build `4e42de0` passed (2026-09-28). CS2 dynamic check and update watch passed on `d817d06`. The bot live test (`livetest.yml`) is manual and has no recorded run on GitHub. |
| Open PRs to land first | #96 "CI: run the fleet link and forfeit live tests in cs2-dynamic (advisory)" — CI only, nice to have. Work in progress on branches, not yet PRs: demo streaming over fleet, practice extras, cvar read + `hello.selftest`. |
| Docs | Draft Ready Up section is `Auto-Tournament/docs#8` (draft, mergeable, "publish with first release"). Its installer page says it downloads "the latest release": add the `--version` note if this ships as a pre-release. |
| Migrations | None on this side. First release, so no upgrade path from an earlier Ready Up release. Postgres → JSON migration script ships in core `tools/`. |
| Upgrade path | From the AT CS2 plugin: per server, see above. The platform keeps RCON servers working next to fleet servers (`cs2_servers.transport` defaults to `rcon`). |
| **Owner condition 1: plugin split** | **Done.** Core + 11 components as separate `.so` plugins, hot enable/disable, bundles checked in CI (`check-bundles.sh`: essentials has match + fleet + practice + essentials and no skins). Leftover: the release-notes table (item 3 above). |
| **Owner condition 2: parity** | **Not done.** `docs/PARITY.md` (audited at `d817d06`): 81.4% of 97 rows, 14 missing, 8 partial, only 5 rows "stable". Left: demo upload in fleet mode (M), `server.config` fields, `scrim_when_idle`, fleet path in CI (#96), `reset_cvars_on_series_end` for match cvars, `server.selftest`, `.rcon`, practice extras / lineup library (L). Milestone M1 (platform drives one match) also needs the platform driver, `Auto-Tournament/auto-tournament#422` (draft). |
| **Owner condition 3: real play-test** | **Not done.** Planned on the dev platform once #422 lands: enroll readyup-test, then one Bo1 and one Bo3 with a pause and a restore, and a human match with Sivert. |
| Blockers | Conditions 2 and 3; the pre-release pipeline items 1–2; the platform's version parse (Ready Up sends `"<semver> (<sha>)"` as `versions.core`, the platform's `parseVersion` returns null for that) — fix one side. |

## Compatibility (betas)

| | Ready Up 0.1.0-beta.1 |
|---|---|
| Fleet protocol | v1 only (`plugins/fleet/fleet_proto.h`: `kProtocolVersion = 1`, hello sends `min 1, max 1`) |
| Plugin API | 1.9 (`core/include/readyup/plugin_api.h`) |
| Auto Tournament platform | **≥ 3.0.0-beta.14** for the fleet link (the gateway, `/api/fleet/ws`, is not in beta.13). Platform speaks protocol 1–1. Match assignment needs the fleet driver (#422, unreleased). Demo upload with a `rus_` fleet token is accepted from beta.14. |
| CS2 Server Manager | Does not install Ready Up (any version). ≥ 1.10.0: live status, match protection, `csm ci`. ≥ 1.10.2: keeps `csgo/readyup` data across game updates. ≥ 1.11.0 (next): hands the platform's license key to Ready Up (`readyup_license_key` in `cfg/readyup_license.cfg`), which this Ready Up reads. |
| Auto Tournament CS2 plugin | Never on the same server as Ready Up's match plugin. |
| CS2 | The build CI verified is printed in the release notes. |
