# Live test

`scripts/livetest/run.sh` plays a short bot-only match (or, with `--scrim`, a bot-only
scrim) on the readyup-test CS2 server and checks Ready Up's state changes in the
console log. Nobody has to join.

```bash
scripts/livetest/run.sh                    # on the cs2 box, as sivert: match mode
scripts/livetest/run.sh --scrim            # bots-only scrim: warmup -> countdown -> knife -> pick -> live
scripts/livetest/run.sh --side switch      # pick sides from the console instead of the timeout
scripts/livetest/run.sh --forfeit          # match + pauses + team-left forfeit (see below)
scripts/livetest/run.sh --out /tmp/lt      # keep console-capture.log + result.json
```

Exit codes: `0` pass, `1` fail, `2` no verdict: the server is busy (a human is
connected), down, or was restarted by someone else during the run. It prints a table of steps at the end (and writes it to
`$GITHUB_STEP_SUMMARY` when that is set).

## What it does

It uses the server console only: `tmux send-keys` to type commands, and the
`console.log` that the tmux pane is piped into to read results.

### Match mode (default)

| Step | Driven by | Checked by |
|---|---|---|
| preflight | - | tmux session `ru-test` exists (waits up to 5 min if a deploy is restarting it), no human connected since the last boot |
| selftest | `ru selftest` | `[ReadyUp] selftest: PASS` |
| reset | `ru idle`, `bot_kick` | `state: mode=idle ... match=none` |
| match load | `ru match load http://127.0.0.1:<port>/match.json`, served by the script: empty roster, `map_sides: ["knife"]`, `maxRounds: 4`, no overtime, short round cvars | `match-load: match context set ... slug=livetest` |
| warmup | `bot_quota 4` (the load kicks bots) | `state: mode=match_warmup warmup=1` |
| bots | - | `state: ... ct=0+2 t=0+2` |
| countdown | - | **SKIP**: only scrims have a countdown (see `--scrim`) |
| knife | automatic (empty roster counts as all ready) | `knife: starting`, `state: mode=match_knife knife=starting`, then `knife=running` |
| knife winner | the bots fight | `knife: winner=...`, `state: ... knife=pick` |
| pick | `auto`: bots-only winner, sides stay after 3 s. `--side stay/switch`: `ru side ...` | `knife: side picked by ...`, `state: mode=match_warmup golive=pending` |
| match_live | - | `state: mode=match_live` |
| rounds | the bots play | `Team "..." triggered "SFUI_Notice_..." (CT "a") (T "b")`, one round per step |
| halftime | `mp_halftime 1` | a bot changes side after round `maxRounds/2` (falls back to the score swapping) |
| map end | - | `Game Over: ...` |
| postgame | - | `state: mode=postgame` with the match still loaded, before any `idle` |
| idle | - | after the series-end kick delay Ready Up unloads the match: `state: mode=idle match=none` |

It fails right away if `mode` leaves `match_live` before the final round, if the log
shows a crash, or if the tmux session dies. Each step has its own timeout.

### Scrim mode (`--scrim`)

A scrim (no match config) normally needs a ready human on CT/T. The core dev flag
`dev_bots_scrim` lets bots alone run one: with no humans on CT/T and bots on both sides,
Ready Up enters scrim warmup, counts the bots as everyone ready, and skips the empty-scrim
timeout. The test turns it on from the console for the run only
(`ru_dev_bots_scrim 1`) and back to the readyup.cfg value (`ru_dev_bots_scrim cfg`) in
cleanup, before `ru scrim`, so the restored bots do not start a scrim afterwards.

| Step | Driven by | Checked by |
|---|---|---|
| selftest, reset | as in match mode | as in match mode |
| dev flag | `ru_dev_bots_scrim 1` | `dev_bots_scrim: 1 (...)` |
| scrim warmup | `ru scrim`, `bot_quota 4` | `state: mode=scrim_warmup ... dev_bots_scrim=1` |
| bots | - | `state: ... ct=0+2 t=0+2` |
| countdown | automatic (bots count as ready) | `state: mode=scrim_warmup ... countdown=N` |
| scrim created | countdown expires | `state: ... match=scrim:<id>` |
| knife, winner, pick, match_live | as in match mode | as in match mode |

It stops once the scrim is live (a scrim is MR12, too long for a test) and cleans up
with `ru idle`. Scrims enter `postgame` at map end like matches do (for
`restart_delay - 1` seconds, no kick); match mode checks that path.

### Forfeit mode (`--forfeit`)

The match flow up to `match_live` with rules in the match config (`max_tech_pauses_per_team 1`,
`tech_pause_max_seconds 8`, `forfeit_after_seconds 20`, `mp_team_timeout_max 1`,
`mp_team_timeout_time 5`, 30 rounds, no auto team balance), then:

| Step | Driven by | Checked by |
|---|---|---|
| team names | `mp_teamname_1`, `mp_teamname_2` | `LiveTestA` / `LiveTestB` (swapped after a knife `switch`); also in match mode |
| tech pause | `ru tech team1` | `pause: technical by team1`, then `pause: ended (technical pause time is up)` |
| tech limit | `ru tech team1` again | `pause: technical refused for team1 (1/1 used)` |
| tactical timeout | `ru tac team2` (`timeout_terrorist_start`) | `pause: tactical timeout by team2`, then `pause: ended (tactical timeout over)` |
| forfeit countdown | `bot_quota_mode normal`, `bot_join_team T`, `bot_kick "<name>"` per CT bot | `forfeit: team1 has nobody connected` |
| cancel | `bot_add_ct` | `forfeit: team1 is back` |
| forfeit | the CT bots kicked again, 20 s | `forfeit: team1 (team_absent) forfeits map 1`, then postgame and idle |

Cleanup also restores `bot_join_team any`, `mp_autoteambalance 1` and the old `bot_quota_mode`.

### Knife round time

Bots do not knife each other, so a bots-only knife round always runs to the time
limit. With `dev_bots_ready` or `dev_bots_scrim` on and no human on CT/T, Ready Up sets
the knife round time to 0.5 min (`knife: dev flag on and no humans on CT/T ...`); the
knife step shows `[dev knife time 0.5 min]` when that happened. `live.cfg` restores the
real round time before going live. Servers without dev flags are unchanged.

Afterwards it always puts the server back the way it was: `ru idle`, then `ru scrim`
(so scrims start again on their own) and the old `bot_quota`. It never restarts the
server. Use `--boot` to start it if the session is still missing after the wait.

Options (also as env vars, see `--help`): `--scrim` (`LIVETEST_MODE=scrim`),
`--max-rounds` (even, default 4), `--bots-per-side` (2), `--side auto|stay|switch`,
`--timescale N` (sets `host_timescale` with `sv_cheats 1`, but only once the match is
live, because Ready Up's restart checks use wall-clock time), `--round-timeout`,
`--map`, and `RU_SSH` / `RU_TARGET` / `RU_SESSION` (same as `scripts/dev-deploy.sh`).

## Fleet (platform link) test

`scripts/livetest/fleet_livetest.py` is the step-3 fleet test (docs/FLEET.md): a mock platform
(`fleet_mock_platform.py`, Python `websockets` + `jsonschema` in a `python:3.12-slim` container on
127.0.0.1:18095-18097) enrolls fleet.so, assigns a bot match, checks every frame against
`plugins/fleet/protocol/v1`, and drives fencing, `match.update`, pause / unpause, `exec`, round
backups + `restore_round`, the offline auto-pause (75 s outage, `offline_pause_minutes=1`),
`end_match` and `match.unassign`, then a failover resume (`match.assign` with `resume`: map 2 of
3 from the run's round backup, series 1-0) and the admins cache version (`admins.set` rev ->
`admins_rev` in snapshots and the next `hello`). It checks that `live_rev` goes up by one per
message and that the patches rebuild the snapshots. Options: `--play-out` (natural map end:
`map_result` with MapStats, `series_end`), `--from-scrim` (assign while a bots-only scrim runs: D16
hand-over), `--no-offline`, `--no-resume`, `--save-backup FILE` / `--resume-only --resume-backup
FILE` (resume from a backup of an earlier run), `--map ws:<id>` (a workshop map), `--out DIR`
(frames.json, console.log, mock.log), `--save-examples DIR`.

It writes `csgo/cfg/ReadyUp/fleet.cfg` for the run and afterwards moves it and fleet.so's data dir
(`csgo/readyup/plugins/fleet/`) to `~/readyup-test/.fleet-livetest/<time>/` and reloads fleet.so,
so the server is standalone again.

## CI

`.github/workflows/livetest.yml` runs this on a self-hosted runner labelled
`readyup-live` (manual dispatch only; input `mode` = match or scrim). **That runner does
not exist yet.** Register one on the cs2 box, as a user that can
`ssh -o BatchMode=yes cs2servermanager@localhost`, before dispatching. Until then the
job just waits in the queue.
