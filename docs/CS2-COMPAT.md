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
```

| Stage | What it proves | Status |
|---|---|---|
| `static` | Every signature, RTTI name, vtable slot, layout and hook site in `gamedata/engine-surface*.json` resolves in the new `libserver.so` | this workflow |
| `selftest` / `live` | A real server boots the new build with every plugin and passes `ru selftest` / the bot live test | planned (self-hosted runner) |

Because the dynamic stages don't exist yet, a clean static run is reported as **warn /
"static ok" (yellow): static pass, live pending**. `pass` / "compatible" (green) needs every
component to pass every stage.

## Components

| id | Checked statically by | Until the dynamic stage exists |
|---|---|---|
| `core` | `engine-surface.json` (signature, rtti, vtable, hook_site, layout) | + a pending `selftest` check |
| `skins` | `engine-surface.skins.json` | + a pending `selftest` check |
| `match`, `practice`, `essentials`, `midas`, `whitelist`, `fleet` | nothing: they never touch the engine (static: n/a, loaded at runtime) | `pending`, one pending `selftest` check |

A new `engine-surface.<id>.json` fragment automatically becomes its own component `<id>`. A
failing check fails its component; a checker that exits abnormally without a `FAIL` line fails
`core`. SKIP lines (entries only checkable at runtime, other modules) are not counted.

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
- `trigger`: `build_change` (new buildid), `surface_change` (gamedata changed on master),
  `manual` (dispatch with `force`). `nightly` and `release` are reserved.
- Progress events (`queued`, `checking`) have `overall: "checking"`, every component
  `checking` with no checks, and `finished_at: null`. `cs2.patch` is `""` until the binaries
  are fetched (so it is empty in `queued`). `no_verdict` (the run broke before a verdict) has
  components `pending`; it is POSTed but not written to `cs2-build`, so the last real verdict
  stays and the next run retries.

`badge.json` is a [shields.io endpoint](https://shields.io/badges/endpoint-badge):
`{"schemaVersion":1,"label":"CS2 1.41.8.5","message":"compatible|static ok|incompatible|checking","color":"brightgreen|yellow|red|blue"}`.

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
