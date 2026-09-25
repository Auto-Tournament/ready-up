# Handoff: cloud session 2026-09-25/26

This is the working list for the next agent across the Auto-Tournament repos. It has no hostnames, IPs, tokens or credentials: ask Sivert for those. Update this file (branch, then PR) as items get done.

## Ground rules (still apply)

- Branch, then PR, then CI green, then squash-merge. An agent may merge its own green PRs.
- Never force-push, rewrite history, create releases or tags, or change repo visibility.
- Never set a GSLT (`sv_setsteamaccount`).
- No production URLs, server IDs, tokens or DB credentials in code, tests or configs.
- Agents can't reach the game servers. In-game checks go on a "Needs Sivert in-game" list and are never claimed as verified.
- csm servers 1–3 on Sivert's box are live production: never restart or update them.
- Sivert prefers short replies and clickable options for decisions.
- Repos in scope: ready-up, auto-tournament (the platform), cs2-server-manager (csm), docs and website.

## Merged this session

- **ready-up #26, #31–#60:**
  - The plugin split: core plus the match, practice, essentials, skins, midas, whitelist, fleet and hello plugins.
  - `.ru <main> <sub>` commands and `ru perf`.
  - Plugin API 1.4–1.6: Workshop download progress, `entity_remove`, and one center panel per player with priorities.
  - Welcome card: waits for the player to spawn, then waits 5 s so CS2's "Match started" message doesn't cover it, and stays up 8 s. The knife panel stays up 10 s.
  - Idle is a waiting room with no rounds, and scrim warmup resets the score. Warmup has no weapon drops and no bomb.
  - Esports: GOTV is required at go-live, and there is an `auto_5v5` pause.
  - Midas steps B and C, and the packaging check.
  - A map change card followed by the Workshop download bar.
  - The `compat.json` report, the README badge and the fast CS2 poller (#55).
- **Platform:**
  - #408: map types from Workshop tags, type pills and sections on the Maps page, and the tournament map-type rule.
  - #409: "Update all" loaders.
  - #410: the compatibility page. **To be removed again, see below.**
  - #411: a duplicate Steam ID now gives a clear 409.
- **csm:**
  - #55: user mode. After `csm setup-host` has been run once as root, no sudo is needed.
  - #49: the live Ready Up fleet table, restart holds during a match, and the status port at game port +7.
- **docs #8** (draft): the Ready Up section, updated to ready-up master. Keep it a draft until the Ready Up release.

## Open or in flight

Check each one; merge it if green, fix it if red.

- **Platform #412:** Deadlock pinned to Q126042383, and every other built-in pinned to its Wikidata id. Also removes "Popular here" and shows only icons in the picked-games bar.
- **Platform #386 (fleet link, step 1):** Sivert reviewed and approved it. It was being brought up to date with main; merge it once green.
- **website `feat/compatibility-page`:** `/compatibility` on autotournament.gg (route handlers, JSON store, SSE, GitHub fallback). After it merges:
  - Remove the compat feature from the platform again: revert #410's code. Its tables were never released.
  - Point the ready-up repo variable `COMPAT_INGEST_URL` at the website's `/api/compat/events`.
  - Set `COMPAT_INGEST_TOKEN` on both the website and ready-up.
- **ready-up `feat/golive-card-admin-call`:**
  - A ~10 s "LIVE" card at go-live listing `.p`, `.up`, `.tech` and `.admin`.
  - `.admin [message]` with a per-player cooldown, an alert to in-game admins, and an `admin_called` webhook: `call_id`, `player` {`steamid64`, `name`, `team`, `side`}, `message`, `called_at`.
- **Platform `local/admin-calls`:** persistent admin toasts with a sound until "Mark resolved", driven by `admin_called`. The branch may be local only if the session ended, so check whether it was pushed. It must match the Ready Up payload above.
- **ready-up `feat/deathmatch-plugin`:**
  - FFA and TDM on CS2 deathmatch, with kill or time limits, a leaderboard HUD and spawn protection.
  - `.ru dm ffa|tdm|off`.
  - Default maps per mode in essentials.
  - Full bundle only.
- **docs #1 (MatchZy naming):** up to date and builds. Its own text says to merge when platform 3.0 ships.
- **csm #45 (AT CS2 2.0.0 rename):** parked. The agreed plan:
  1. Freeze the old CounterStrikeSharp plugin.
  2. The platform supports both server kinds (#386 plus the fleet driver).
  3. At Ready Up 1.0, announce a deprecation date and publish a migration guide.
  4. Archive the old plugin and close #45.

## Todo, in priority order

1. Merge the green items from "in flight" and fix the red ones.
2. **Center-card queue in the core (Ready Up plugin API 1.7):**
   - A per-player queue of one-off cards: welcome, go-live, admin call, votes, map change.
   - FIFO within a priority, and every card gets its full time.
   - A higher-priority card cuts in; the interrupted card resumes with its remaining time.
   - The HUD fills the gaps.
   - Priorities (in `docs/HUD.md`): ALERT 90, MENU 80, NOTICE 70, HUD 50, INFO 10.
3. **Platform: default map per mode:**
   - A "Set as default for…" action on map cards: practice, warmup, ffa, tdm, retakes, arenas, aim. Only offer modes that aren't set to this map already.
   - A "Default: X" pill on the card.
   - Used when a server's mode or preset is set.
   - Ready Up essentials has `default_maps` if the deathmatch PR merged.
4. **More Ready Up modes after deathmatch:** retakes, 1v1 arenas and aim maps, following the deathmatch plugin's pattern.
5. **Platform fleet step 2 and later:**
   - `cmd` / `cmd.result` so the platform can call `plugins.set`, `whitelist.set` and `practice.set`. The Ready Up side is done (ready-up #39).
   - The fleet driver: link fleet servers to `cs2_servers`.
   - On top of that: server presets (Practice / Match / Official Valve), a whitelist UI with Steam friends, plugin toggles, and a skin store (coins earned by performance, no money).
6. **csm host agent (PR 2):**
   - `csm agent` as a `systemd --user` service, with a WebSocket to the platform's `/api/fleet/host` using an `rhs_` token.
   - It handles start, stop, create, update, logs and health, so servers can be spun up and down.
   - Start servers outside the agent's cgroup (`systemd-run --user --scope`), or restarting the agent kills them.
   - Needs #386 and a host channel on the platform.
7. **Compatibility checker, dynamic stage:**
   - A self-hosted GitHub runner `readyup-live` on Sivert's box.
   - A separate `ru-ci` CS2 instance: about 60 GB, no GSLT, `sv_lan 1`.
   - `cs2-dynamic.yml`: steamcmd delta update, selftest-and-quit, livetest.
   - Sivert has to register the runner.
8. **Ready Up leftovers:**
   - A WASD `.ru` menu (center HTML plus button reading, MENU priority).
   - Kill and respawn for `.skins reload`.
   - HUD design guidelines, once Sivert sends screenshots of `.ru hud test 8`–`11` and recordings of `.ru hud anim 64` / `32` / `16` (count the distinct frame numbers per second).
   - Coach support.
   - A full match play-test with Sivert, then release prep.
9. **Slow server frames:** not yet diagnosed. Sivert runs `ru perf` after a stutter and shares the `perf:` log lines.

## Needs Sivert

- **Deploy to the test box.** `--restart` only deploys the core, so the plugin loop is needed too:
  ```
  cd ~/ready-up && git pull
  scripts/dev-deploy.sh --restart
  for p in match essentials practice skins fleet; do scripts/dev-deploy.sh --plugin "$p"; done
  ```
- **In game:**
  - The welcome card stays about 8 s after "Match started", and the score stays 0:0 on join.
  - `.ru hud anim 64 10` (record it).
  - The knife panel stays up 10 s.
  - A Workshop map change shows the card, then the download bar.
  - `.ru map change` works for admins.
- **Compatibility:**
  - A fine-grained PAT for the ready-up poller: `CS2_POLL_TOKEN`, Actions read/write, stored on an always-on box.
  - Run the "cs2-update-watch" workflow once manually with `force`.
- **csm user mode on the live box:**
  1. Install the new binary.
  2. Run `sudo csm setup-host --skip-deps --skip-linger`. Never run it without `--skip-deps`: apt could upgrade tmux under the running servers.
  3. Check with `sudo -iu cs2servermanager csm status`.
