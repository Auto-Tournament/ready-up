# CS2 update checks

Every CS2 update is checked against Ready Up's engine surface, usually within minutes of the
update. The result is published as `compat.json` (for the Auto Tournament platform's live
page) and `badge.json` (the README badge), both on the `cs2-build` branch.

(Running next to Metamod or CounterStrikeSharp is covered in [COMPATIBILITY.md](COMPATIBILITY.md).)

## Pipeline

```
cs2-poll.sh (every 2-3 min, any box) ─┐ workflow_dispatch
GitHub cron */15 (fallback, can lag) ─┴─> cs2-update-watch.yml
    check   : buildid (api.steamcmd.net) or gamedata changed?  ── POST queued
    verify  : fetch 5 CS2 files (DepotDownloader, anonymous)    ── POST checking
              readyup_sigcheck + readyup_hookcheck (base surface + every fragment)
              compat-report.py -> compat.json + badge.json
    report  : issue / Discord / commit comment (as before)       ── POST pass|warn|fail
              state.env + compat.json + badge.json -> branch cs2-build
    no-verdict (verify broke without a result)                   ── POST no_verdict
        │ new static verdict (not fail)
        v
cs2-dynamic.yml (self-hosted runner `readyup-live`; also nightly + workflow_dispatch; off until
                 repo variable CS2_DYNAMIC_ENABLED=true)
    build   : Full bundle in the sniper SDK (GitHub-hosted)
    live    : steamcmd app_update 730 -> install.sh --zip (Full) -> boot +sv_lan 1
              selftest: READYUP_SELFTEST_AND_QUIT -> readyup_selftest.txt  ── POST + cs2-build (stage selftest)
              live:     scripts/livetest match + scrim (bots)            ── POST + cs2-build (stage live)
```

| Stage | What it proves | Status |
|---|---|---|
| `static` | Every signature, RTTI name, vtable slot, layout and hook site in `gamedata/engine-surface*.json` resolves in the new `libserver.so` | this workflow |
| `selftest` | A real server boots the build with every plugin: `ru selftest` passes, no plugin is disabled, every plugin's schema fields and game events exist | `cs2-dynamic.yml` |
| `live` | The bot live test (`scripts/livetest`: a match to map end, then a scrim to live) passes: tournaments are safe | `cs2-dynamic.yml` |

A clean static run is reported as **warn / "static ok" (yellow): static pass, live pending**.
`pass` / "compatible" (green) needs every component to pass every check, which only the
dynamic stages can confirm.

## Components

| id | Static checks | Dynamic checks |
|---|---|---|
| `core` | every entry of `engine-surface.json` (signature, rtti, vtable, hook_site, layout) | `selftest`: the core's own `ru selftest` lines |
| `skins` | `engine-surface.skins.json` + the core entries in `plugins/skins/needs.json` | `schema`, `event`, `selftest` |
| `match`, `practice`, `essentials`, `midas`, `whitelist`, `deathmatch`, `fleet` | the engine-surface entries in `plugins/<id>/needs.json` | `schema`, `event` (when it needs any), `selftest`; `match` also `livetest` |

A new `engine-surface.<id>.json` fragment automatically becomes its own component `<id>`. A
failing check fails its component, and an entry needed by several plugins fails each of them (a
broken `Host_Say` fails core, match, practice, fleet and skins; essentials, whitelist, midas and
deathmatch do not use chat commands and stay green). A checker that exits abnormally without a
`FAIL` line fails `core`. SKIP lines (entries only checkable at runtime, other modules, such as
the `CSchemaSystem` rtti) are not counted. Schema and event needs stay `pending` until the
`selftest` stage confirms them.

## Plugin needs

Every plugin (not the `hello` example) declares what it needs from CS2 in
`plugins/<name>/needs.json`, shipped next to it as `csgo/readyup/plugins/<name>.needs.json`:

```json
{"schema_version": 1, "plugin": "match",
 "api": ["chat_all", "schema_offset", "subscribe_game_event", "..."],
 "surface": ["UTIL_ClientPrintAll", "Host_Say", "CGameEventManager_Init", "..."],
 "schema": ["CBasePlayerController.m_steamID|CCSPlayerController.m_steamID"],
 "schema_optional": ["CCSPlayerController.m_iKills"],
 "events": ["round_start", "round_end", "..."]}
```

- `api`: the engine-facing `ru_api` members the plugin calls. `API_SURFACE` in
  `scripts/ci/compat-report.py` maps each one to the engine-surface entries the core needs to
  serve it (`chat_all` -> `UTIL_ClientPrintAll`, `register_chat_command` -> `Host_Say`,
  `schema_offset` -> the `CSchemaSystem` rtti, `entity_*` -> `UTIL_Remove` and the entity
  functions, ...); members that touch no engine surface (config, stash, interfaces) are left out.
- `surface`: those engine-surface entries (function, hook, rtti, vtable slot or layout names from
  `gamedata/engine-surface*.json`). They are the plugin's static verdict.
- `schema` / `schema_optional`: `Class.m_field` read through `schema_offset`; `A.m_x|B.m_x` when
  the plugin tries several classes (one is enough). A missing required field fails the plugin, a
  missing optional one (stats, cosmetics) only warns.
- `events`: game events it subscribes to (`subscribe_game_event`). A missing one warns.

The manifests cannot drift: `python3 scripts/ci/compat-report.py needs-check` (Build workflow,
and `tests/test_compat_report.py`) fails when a plugin's source calls `schema_offset` with a
`"Class", "m_field"` literal pair (also through wrappers like `F({"A", "B"}, "m_x")`), uses any
`"m_..."` field literal, subscribes to a game event, or calls an engine-facing `ru_api` member
that its `needs.json` does not declare, or when `surface` misses an entry its `api` implies or
names one that no engine-surface file has. `compat-report.py needs-derive <name>` prints the
manifest the source implies (a starting point for a new plugin).

### The core enforces them

When the plugin host loads a plugin that has a `<name>.needs.json`
(`core/src/readyup/plugin_needs.cpp`):

- a needed engine-surface entry that did not resolve on this build, or a required schema field
  with every alternative missing: the plugin is **not loaded**. The console shows
  `WARN plugin[<name>] disabled: missing <X> after CS2 build <N>`, `ru plugin list` and
  `ru selftest` show the reason (a `WARN <name>: disabled` line, not a selftest failure), in-game
  admins get it in chat once per map, and fleet reports it in `hello.plugins_disabled`
  (`[{name, reason}]`, `plugins/fleet/protocol/v1/messages/hello.json`).
- a missing optional schema field or an unknown game event: loaded, with a warning.
- a need that cannot be checked yet (schema system or event manager not up): loaded.
- no `needs.json` (older plugins, third-party plugins, `hello`): loaded as before.

A missing `required` core entry still disables Ready Up entirely, as before. `ru selftest` lists
every need of every plugin in a `[plugin needs]` section
(`OK|WARN|PEND need <plugin> <surface|schema|schema_optional|event> <entry>`), which is what the
`selftest` stage reads.

## compat.json (schema 1)

Served from `https://raw.githubusercontent.com/Auto-Tournament/ready-up/cs2-build/compat.json`
and POSTed as progress events (below). Keep this contract stable; bump `schema` on breaking
changes.

```json
{"schema":1,
 "cs2":{"buildid":"25537370","patch":"1.41.8.5"},
 "readyup":{"version":"0.1.0-dev.7f61b71","commit":"<sha>"},
 "run":{"id":"<github run id>","url":"<run url>","trigger":"build_change|surface_change|nightly|release|manual",
        "stage":"static|selftest|live","state":"queued|checking|pass|warn|fail|no_verdict",
        "started_at":"ISO8601Z","finished_at":"ISO8601Z|null"},
 "overall":"pass|warn|fail|checking|no_verdict",
 "components":[{"id":"core","name":"Core","status":"pass|warn|fail|pending|checking",
   "checks":[{"kind":"signature|rtti|vtable|hook_site|layout|schema|event|selftest|livetest",
              "status":"pass|warn|fail|pending","passed":9,"total":9,"failures":["Name: detail"]}]}],
 "checked_at":"ISO8601Z"}
```

- `overall`: `fail` if any component fails; `pass` only if every component and check passes;
  otherwise `warn` (static pass, live pending). `run.state` equals `overall` for a result.
- `trigger`: `build_change` (new buildid; for the dynamic stages: after a new static verdict),
  `surface_change` (gamedata or a plugin's `needs.json` changed on master), `manual` (dispatch),
  `nightly` (the dynamic stages' nightly run). `release` is reserved.
- `run.stage`: `static` from the watch; `selftest` then `live` from `cs2-dynamic.yml`. A dynamic
  document keeps the static checks of the last static verdict and replaces the dynamic ones, so
  the newest document on `cs2-build` is always the whole picture.
- Progress events (`queued`, `checking`) have `overall: "checking"`, every component
  `checking` with no checks, and `finished_at: null`. `cs2.patch` is `""` until the binaries
  are fetched (so it is empty in `queued`). `no_verdict` (the run broke before a verdict) has
  components `pending`; it is POSTed but not written to `cs2-build`, so the last real verdict
  stays and the next run retries.

`badge.json` is a [shields.io endpoint](https://shields.io/badges/endpoint-badge):
`{"schemaVersion":1,"label":"CS2 1.41.8.5","message":"compatible|static ok|incompatible|checking","color":"brightgreen|yellow|red|blue"}`.

Local run of the dynamic report: `python3 scripts/ci/compat-report.py dynamic --stage live
--base compat.json --selftest readyup_selftest.txt --livetest match=0 --livetest scrim=0 --out-dir .ci`.

Local run (after `VERIFY_RAW_DIR=.ci/raw scripts/ci/verify-cs2.sh ...`):
`python3 scripts/ci/compat-report.py result --raw-dir .ci/raw --build-env <cs2>/cs2-build.env --out-dir .ci`.
Tests: `python3 -m unittest tests.test_compat_report` (also run by the Build workflow).

## Progress events

| Setting | Kind | Value |
|---|---|---|
| `COMPAT_INGEST_URL` | repo **variable** | the platform's ingest endpoint |
| `COMPAT_INGEST_TOKEN` | repo **secret** | sent as `Authorization: Bearer <token>` |

With the variable unset nothing is sent. A failed POST is logged as a warning and never fails
the check. Each event is the full compat.json document (`Content-Type: application/json`); the
receiver should key on `run.id` and treat later events as replacing earlier ones.

## Poller (fast detection)

GitHub runs `schedule` workflows best effort, often hours late during load. `scripts/ci/cs2-poll.sh`
closes that gap: it reads the public buildid and `state.env` on `cs2-build` and, on a change,
starts the workflow with `workflow_dispatch` (once per buildid per 30 minutes). It also asks
Steam's keyless `ISteamApps/UpToDateCheck?appid=730&version=<patch>` and logs when Steam
already reports the recorded patch as outdated (informational only). The GitHub cron stays as
the fallback.

Token: a **fine-grained personal access token**, resource owner `Auto-Tournament`, repository
access *only* `ready-up`, permission **Actions: Read and write** (metadata read is implied).
Store it only on the poller box.

Setup on any always-on Linux box with `bash`, `curl` and `python3` (systemd user timer):

```sh
git clone --depth 1 https://github.com/Auto-Tournament/ready-up ~/ready-up-poll
install -m 600 /dev/null ~/.config/readyup-cs2-poll.env
echo 'CS2_POLL_TOKEN=<fine-grained PAT>' >> ~/.config/readyup-cs2-poll.env

mkdir -p ~/.config/systemd/user
cat > ~/.config/systemd/user/readyup-cs2-poll.service <<'EOF'
[Unit]
Description=Ready Up: dispatch the CS2 update watch on a new CS2 build
[Service]
Type=oneshot
EnvironmentFile=%h/.config/readyup-cs2-poll.env
ExecStart=%h/ready-up-poll/scripts/ci/cs2-poll.sh
EOF
cat > ~/.config/systemd/user/readyup-cs2-poll.timer <<'EOF'
[Unit]
Description=Poll CS2 buildid every 2 minutes
[Timer]
OnBootSec=1min
OnUnitActiveSec=2min
RandomizedDelaySec=20
[Install]
WantedBy=timers.target
EOF
systemctl --user daemon-reload
systemctl --user enable --now readyup-cs2-poll.timer
loginctl enable-linger "$USER"      # keep user timers running without a login session
journalctl --user -u readyup-cs2-poll -f
```

Or with cron: `*/2 * * * * . $HOME/.config/readyup-cs2-poll.env && $HOME/ready-up-poll/scripts/ci/cs2-poll.sh >>$HOME/.cache/cs2-poll.log 2>&1`
(the env file needs `export CS2_POLL_TOKEN=...` for cron). Update the clone with `git pull` now and then. `DRY_RUN=1 scripts/ci/cs2-poll.sh`
checks everything except the dispatch. Other settings (`CS2_POLL_REPO`, `CS2_POLL_REF`,
`CS2_POLL_COOLDOWN`, ...) are listed at the top of the script.

## Dynamic stage (self-hosted runner)

`.github/workflows/cs2-dynamic.yml` runs after every watch run that recorded a new, non-failing
static verdict (`workflow_run`), nightly, and on `workflow_dispatch`. It is **off** until the
repo variable `CS2_DYNAMIC_ENABLED` is `true`. With it on and no runner registered, the `live`
job waits in the queue until one is.

1. `build` (GitHub-hosted, sniper SDK): the Full bundle of the current default branch.
2. `live` (`runs-on: [self-hosted, readyup-live]`, environment `cs2-dynamic`):
   - `steamcmd +force_install_dir $CS2_CI_DIR +login anonymous +app_update 730` (a delta when
     current; skipped with a notice when steamcmd is not on the runner and csm keeps it updated);
   - `install.sh --dir $CS2_CI_DIR --zip ready-up-full-*.zip --yes --accept-license=noncommercial full`;
   - boot `game/bin/linuxsteamrt64/cs2 -dedicated -port $CS2_CI_PORT +sv_lan 1 +map de_dust2`
     with `READYUP_SELFTEST_AND_QUIT=1` (no GSLT, never `sv_setsteamaccount`) and turn
     `readyup_selftest.txt` into the `selftest` stage: core lines -> `core`, `<plugin>: ...` lines,
     `WARN <plugin>: disabled` and a plugin missing from the loaded list -> that plugin's
     `selftest`, the `[plugin needs]` lines -> its `schema` / `event` checks;
   - `scripts/livetest/run.sh --ssh '' --target <tmp> --boot` twice (match, then `--scrim`) on
     the same install -> `match`'s `livetest` check (exit 2 = no verdict: stays pending);
   - POST each stage to `COMPAT_INGEST_URL` and commit compat.json + badge.json to `cs2-build`
     (`scripts/ci/cs2-watch-state.sh publish`). A run that breaks before the selftest verdict
     POSTs `no_verdict` and records nothing.

### Runner setup (one time)

Recommended: on the CS2 box, as the `cs2` user (never root), with a registration token from
Settings -> Actions -> Runners -> New self-hosted runner:

```sh
csm ci setup --token <registration token>
```

It installs a dedicated CS2 server copy for CI (kept updated by csm), registers the runner with
the label `readyup-live`, and writes `CS2_CI_DIR` (the install root, containing `game/`) and
`CS2_CI_PORT` (default 27095) into the runner's `.env`. The workflow fails with a clear message
when either is missing.

Manual fallback (same result, as a dedicated non-root user; about 60 GB of disk for CS2 plus the
runner):

```sh
sudo useradd -m -s /bin/bash cs2ci            # once, as an admin; everything else as cs2ci
sudo -iu cs2ci
# 1. CS2 dedicated server, anonymous (no GSLT is used or needed: the server runs +sv_lan 1)
mkdir -p ~/steamcmd && cd ~/steamcmd
curl -sSL https://steamcdn-a.akamaihd.net/client/installer/steamcmd_linux.tar.gz | tar xz
./steamcmd.sh +force_install_dir ~/cs2-ci +login anonymous +app_update 730 validate +quit
# 2. GitHub runner (Settings -> Actions -> Runners -> New self-hosted runner shows the current
#    download URL and token)
mkdir -p ~/actions-runner && cd ~/actions-runner
#    ... download + extract the runner as shown there, then:
./config.sh --url https://github.com/Auto-Tournament/ready-up --token <registration token> \
  --labels readyup-live --name "$(hostname)-readyup-live" --unattended
printf 'CS2_CI_DIR=%s\nCS2_CI_PORT=27095\n' "$HOME/cs2-ci" >> .env
exit
# 3. back as the admin: a systemd service that runs the runner as cs2ci
cd ~cs2ci/actions-runner && sudo ./svc.sh install cs2ci && sudo ./svc.sh start
```

The runner needs `tmux`, `python3`, `curl`, `unzip` and `bash` 4+; `steamcmd` on `PATH` (or in
`~/steamcmd/steamcmd.sh`) lets the workflow update CS2 itself.

Then, in the repository settings:

1. Environments -> New environment `cs2-dynamic`: deployment branches **only the default
   branch** (optionally required reviewers). Add `COMPAT_INGEST_TOKEN` there or keep the repo
   secret.
2. Variables -> `CS2_DYNAMIC_ENABLED` = `true` (unset / anything else keeps the workflow off).
3. Actions -> Run workflow "CS2 dynamic check" once to see the first verdict.

Security: the runner executes whatever the workflow checks out, so the workflow has only
`schedule`, `workflow_dispatch` and `workflow_run` triggers, **never `pull_request`**, and the
protected environment keeps other branches off it. Keep it off public forks' reach (the repo
setting "Require approval for all outside collaborators" stays on), run it as a non-root user
with no sudo, and on a box whose CS2 install is only for CI (`CS2_CI_PORT` must not collide with
a live server).
