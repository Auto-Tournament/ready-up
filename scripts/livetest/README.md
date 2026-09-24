# Live test

`scripts/livetest/run.sh` plays a short bot-only match on the readyup-test CS2 server
and checks Ready Up's state changes in the console log. Nobody has to join.

```bash
scripts/livetest/run.sh                    # on the cs2 box, as sivert
scripts/livetest/run.sh --side switch      # pick sides from the console instead of the timeout
scripts/livetest/run.sh --out /tmp/lt      # keep console-capture.log + result.json
```

Exit codes: `0` pass, `1` fail, `2` no verdict: the server is busy (a human is
connected), down, or was restarted by someone else during the run. It prints a table of steps at the end (and writes it to
`$GITHUB_STEP_SUMMARY` when that is set).

## What it does

It uses the server console only: `tmux send-keys` to type commands, and the
`console.log` that the tmux pane is piped into to read results.

| Step | Driven by | Checked by |
|---|---|---|
| preflight | - | tmux session `ru-test` exists (waits up to 5 min if a deploy is restarting it), no human connected since the last boot |
| selftest | `ru selftest` | `[ReadyUp] selftest: PASS` |
| reset | `ru idle`, `bot_kick` | `state: mode=idle ... match=none` |
| match load | `ru match load http://127.0.0.1:<port>/match.json`, served by the script: empty roster, `map_sides: ["knife"]`, `maxRounds: 4`, no overtime, short round cvars | `match-load: match context set ... slug=livetest` |
| warmup | `bot_quota 4` (the load kicks bots) | `state: mode=match_warmup warmup=1` |
| bots | - | `state: ... ct=0+2 t=0+2` |
| countdown | - | **SKIP**: only scrims have a countdown, and a scrim needs a ready human (see below) |
| knife | automatic (empty roster counts as all ready) | `knife: starting`, `state: mode=match_knife knife=starting`, then `knife=running` |
| knife winner | the bots fight | `knife: winner=...`, `state: ... knife=pick` |
| pick | `auto`: bots-only winner, sides stay after 3 s. `--side stay/switch`: `ru side ...` | `knife: side picked by ...`, `state: mode=match_warmup golive=pending` |
| match_live | - | `state: mode=match_live` |
| rounds | the bots play | `Team "..." triggered "SFUI_Notice_..." (CT "a") (T "b")`, one round per step |
| halftime | `mp_halftime 1` | a bot changes side after round `maxRounds/2` (falls back to the score swapping) |
| map end | - | `Game Over: ...` |
| postgame | - | Ready Up clears the match: `state: mode=idle match=none` |

It fails right away if `mode` leaves `match_live` before the final round, if the log
shows a crash, or if the tmux session dies. Each step has its own timeout.

Afterwards it always puts the server back the way it was: `ru idle`, then `ru scrim`
(so scrims start again on their own) and the old `bot_quota`. It never restarts the
server. Use `--boot` to start it if the session is still missing after the wait.

Options (also as env vars, see `--help`): `--max-rounds` (even, default 4),
`--bots-per-side` (2), `--side auto|stay|switch`, `--timescale N` (sets `host_timescale`
with `sv_cheats 1`, but only once the match is live, because Ready Up's restart checks
use wall-clock time), `--round-timeout`, `--map`, and `RU_SSH` / `RU_TARGET` /
`RU_SESSION` (same as `scripts/dev-deploy.sh`).

## CI

`.github/workflows/livetest.yml` runs this on a self-hosted runner labelled
`readyup-live` (manual dispatch only). **That runner does not exist yet.** Register
one on the cs2 box, as a user that can `ssh -o BatchMode=yes cs2servermanager@localhost`,
before dispatching. Until then the job just waits in the queue.

## Not covered: the scrim countdown

The scrim flow (humans `.r` → 5 s countdown → knife) only starts when at least one
human is on CT/T. `dev_bots_ready=1` makes bots count as ready, but a scrim with only
bots never starts (`scrim_flow.cpp`: `ScrimTick` needs `c.total > 0`, and
`ScrimWarmupStepLocked` goes back to idle after 5 s with no humans). This test uses a
loaded match with an empty roster, which covers the knife → pick → live → halftime →
map end path, but not scrim warmup or its countdown.
