# Esports mode (`ruleset: "valve"`) — spec

Status: **spec only, nothing implemented.** This maps Valve's published CS2 tournament rules onto
Ready Up and says what an opt-in "esports" mode would have to do.

## Sources

| Source | What it gives | Pinned at |
|---|---|---|
| [ValveSoftware/counter-strike_rules_and_regs](https://github.com/ValveSoftware/counter-strike_rules_and_regs) `major-supplemental-rulebook.md` | Server convars (L359-477), server mods (L352-354), demos (L356-357), client convars (L343-349), game modifications / inventory (L285-290), coach + staff (L557-564), substitutions (L542-549), pick-ban + side choice (L212-232), match logs (L479-483, L566-587) | [`8594dc0`](https://github.com/ValveSoftware/counter-strike_rules_and_regs/blob/8594dc0cc8f8cd9ea9564af46e453fc673246cbd/major-supplemental-rulebook.md) (rulebook last changed in `a22f91d`, 2025-08-05) |
| same repo, `tournament-operation-requirements.md` | Business/ranking rules (invites, prize money, publication). **No in-game rules.** | `8594dc0` |
| Premier defaults the rulebook points at: `cfg/gamemode_competitive.cfg` then `cfg/gamemode_competitive_tmm.cfg` (shipped with the dedicated server) | The base every value below starts from | read from the SteamDB mirror [SteamTracking/GameTracking-CS2 `760e69c`](https://github.com/SteamTracking/GameTracking-CS2/tree/760e69c1f19a8728ef109bbc14cfc5ef2de9823b/game/csgo/cfg) (2026-09-24); the server's own copy is authoritative |

What Valve does **not** publish: a knife round, pause counts beyond the Premier cvars, a coach
slot, a `tv_delay` value (it is on the "any value" list), a ban on agents, stickers, gloves or
skins, or bind/script rules. Anything in this doc beyond the rulebook is marked **TO option**
(tournament organizer choice, not Valve).

## The rule, in one sentence

> "All server convars are to be set to Premier defaults (by first loading
> cfg/gamemode_competitive.cfg and then loading cfg/gamemode_competitive_tmm.cfg) except for the
> following exceptions." (rulebook L360)

So esports mode does **not** copy Premier values into a Ready Up cfg. It execs Valve's two files
and then applies Valve's exception list. When Valve changes Premier, esports mode follows with no
Ready Up change.

## Rule table

Category **A** = cvar/cfg value only. **B** = needs plugin logic. **C** = not enforceable
server-side (admin/process rule).

### A: convars

Valve's exact values (L362-418). "RU now" is `cfg/ReadyUp/live.cfg` (or the engine default when
Ready Up does not set it). **⚠** = Ready Up's current default differs.

| Cvar | Valve | RU now | Note |
|---|---|---|---|
| (base) | `exec gamemode_competitive` + `exec gamemode_competitive_tmm` | MatchZy-copied list | ⚠ the root difference, see conflicts below |
| `game_type` / `game_mode` | `0` / `1` | launch args | must be set before the map loads; esports mode checks, does not change mid-map |
| `tv_enable` / `tv_enable1` | `1` / `1` | `tv_enable 1` only when demo recording starts | ⚠ must be on before map load (already noted in `demo_recorder.cpp`) |
| `tv_dispatchmode` | `0` | default | |
| `tv_broadcast` | `1` | default `0` | ⚠ needs `tv_broadcast_url`; for non-Major events this is a TO option, see B |
| `tv_allow_autorecording_index` | `0` | default | Ready Up records with its own `tv_record`; no conflict |
| `tv_relayradio` | `0` | default | |
| `tv_allow_camera_man_steamid` | observer's SteamID | unset | per event, from match config |
| `sv_maxusrcmdprocessticks` | `3` | default | |
| `sv_max_dropped_packets_to_process` | `3` | default | |
| `sv_clockcorrection_msecs` | `0` | default | |
| `sv_steamauth_enforce` | `1` | default | |
| `mp_autokick` | `0` | `0` | same |
| `sv_matchend_drops_enabled` | `0` | default | |
| `sv_damage_print_enable` | `0` | `0` | same |
| `sv_reliableavatardata` | `2` | default | |
| `sv_invites_only_mainmenu` | `1` | default | |
| `mp_spectators_max` | `10` | `20` | ⚠ |
| `sv_matchpause_auto_5v5` | `1` | default | disconnect handling, see B |
| `mp_overtime_limit` | `0` (unlimited OT) | TMM `1` / RU `maxOvertimes` | ⚠ RU's `overtimeMode`/`maxOvertimes`/`damageTiebreak` must be off |
| `mp_warmup_pausetimer` | `1` | `0` | ⚠ RU emulates warmup with `mp_warmuptime 0`, see conflicts |
| `mp_warmuptime` | `60` | `0` | ⚠ same |
| `mp_warmuptime_all_players_connected` | `0` | default | |
| `mp_halftime_pausematch` | `1` | unset (`0`) | ⚠ halftime becomes a pause; see B |
| `mp_competitive_endofmatch_extra_time` | `155` | unset | interacts with RU's series-end kick delay |
| `sv_spec_hear` | `4` | default | |
| `sv_vote_allow_spectators` | `1` | default | |
| `sv_vote_issue_loadbackup_spec_only` | `1` | default | only spectators (admins) may vote a backup load |
| `sv_vote_issue_loadbackup_spec_authoritative` | `1` | default | |
| `sv_vote_issue_pause_match_spec_only` | `1` | default | players cannot vote-pause; see B |
| `sv_vote_creation_timer` / `sv_vote_failure_timer` | `25` / `25` | default | |
| `sv_occlude_players` | `0` | default | |
| `sv_force_transmit_players` / `sv_force_transmit_ents` | `1` / `1` | default | |
| `sv_holiday_mode` | `0` | default | |
| `cash_team_bonus_shorthanded` | `0` | `0` | same (Premier base is `1000`, Valve overrides) |
| `log` | `on` | via match log | |
| `mp_logdetail` / `mp_logdetail_items` / `mp_logmoney` | `3` / `1` / `2` | `mp_logdetail 3` at knife only | ⚠ set in live too |
| `sv_gameinstructor_disable` | `1` | default | |

Premier values Ready Up's `live.cfg` currently **overrides with a different value** (all fixed by
exec'ing Valve's files instead):

| Cvar | Premier (Valve) | RU `live.cfg` |
|---|---|---|
| `mp_freezetime` | `20` (tmm) | `18` |
| `mp_team_timeout_time` | `31` (tmm) | `30` |
| `mp_team_timeout_ot_add_once` | `1` (tmm) | unset |
| `mp_weapons_allow_zeus` | `5` | `1` |
| `mp_respawn_immunitytime` | `-1` | `0` |
| `mp_technical_timeout_per_team` / `_duration_s` | `1` / `120` | unset, and `.tech` bypasses it |
| `mp_overtime_startmoney` | engine default (Premier sets none; `12500` on current builds, check `cvarlist`) | `10000` |
| `bot_quota` | `1` (Premier base; set to `0` after the exec, Premier bot fill is matchmaking-only) | unset |

Cvars Valve lets the TO pick (L421-476), so esports mode may keep Ready Up's values:
`mp_disconnect_kills_players`, `mp_do_warmup_period`, `mp_halftime_pausetimer`,
`mp_overtime_halftime_pausetimer`, `mp_win_panel_display_time`, `spec_replay_enable`,
`sv_allow_votes`, `sv_hibernate_*`, `sv_pausable`, `sv_voiceenable`, `sv_lan`, rates,
`tv_delay`, `tv_delay1`, `tv_*` naming/relay/rate cvars, `tv_autorecord`, `tv_transmitall`.
Ready Up-only cvars not on either list (`mp_backup_round_auto`, `mp_backup_restore_load_autopause`,
`mp_round_restart_delay`, `tv_relayvoice`, `mp_match_end_restart`, `mp_team_intro_time`) are
match-management cvars; keep them, they do not change gameplay.

### B: needs plugin logic

| Rule (source) | Ready Up today | Esports implementation |
|---|---|---|
| **Players' inventory access "should not be modified or augmented"** (L290) | `plugins/skins` gives paints, knives, gloves, agents the player may not own | Skins plugin is **off** in esports mode: `skins` checks `ru_api config_get("ruleset")` (or a core `ru_api ruleset()` getter) at load and on `ru reload`; when `valve`, it applies nothing and restores nothing (players keep their real Steam inventory: their own skins, stickers, gloves, agents are allowed by Valve). Core logs a loud line if `skins.so` is loaded with `ruleset=valve`. |
| **Default agents / no custom cosmetics** (**TO option**, not Valve) | none | Match config `cosmetics: "inventory"` (default, Valve-compatible) \| `"default_agents"`. `default_agents`: on spawn, `entity_set_model(pawn, <map default model for team>)` through the existing skins engine surface (`cosmetics.cpp` path, same frame + next frame). The default models come from a cfg table `cfg/ReadyUp/esports_default_agents.cfg` (per team, per map faction); there is no CS2 server cvar for "default agents" on the current build (verify with `cvarlist model` after each update). Only the model is reset. Gloves and weapon stickers/skins from a real inventory are econ items; clearing them means writing econ fields, which is the thing that gets GSLTs banned, so **not offered**. Ban risk: `SetModel` to a stock model grants no item and edits no econ data; it is lower risk than the skins plugin, but it still needs `engine-surface.skins.json` and is the same call skin changers use. Ship it off by default and document the trade-off. |
| **Pause rules**: tactical timeouts via Premier cvars (`mp_team_timeout_max 3`, `_time 31`, OT `+1`), technical `mp_technical_timeout_per_team 1` × `120 s`, `sv_vote_issue_pause_match_spec_only 1` | `.pause`/`.p`/`.tech` all call `mp_pause_match`, unlimited, both teams unpause (`match_router.cpp:259-277`) | `.tac`/`.pause` → built-in `timeout_ct_start` / `timeout_terrorist_start` for the caller's side (the engine counts and ends it). `.tech` → `mp_pause_match`, counted by Ready Up against `mp_technical_timeout_per_team`, auto-unpause after `mp_technical_timeout_duration_s` unless an admin extends it. Further pauses: admin only (`ru pause`). `pause.type`/`is_tactical`/`pause_time` filled in events (closes PARITY.md P1 item). |
| **Halftime pause** `mp_halftime_pausematch 1` | not set; RU unpause flow does not expect it | Treat the halftime pause as `pause.type="halftime"`: unpause by both teams `.unpause` or admin, no count against either team. |
| **Disconnects** `sv_matchpause_auto_5v5 1` | none | Engine pauses at freezetime when not 5v5. RU must recognise that pause (`pause.type="auto_5v5"`), show it on the HUD, and not require `.unpause` from the short team to be counted as a tactical. |
| **Backups/restores** `sv_vote_issue_loadbackup_spec_only 1` + `_authoritative 1` | restore only on boot recovery; `.ru restore` planned | Restores admin-only (never a player vote), always followed by a pause (`mp_backup_restore_load_autopause 1` already set). |
| **Side selection** from pick-ban: bo1 Team B picks side; bo3 Team B map 1, Team A map 2, Team B map 3 (L215-232) | `map_sides: "knife"` supported; scrims default `scrim_knife=1` | Esports mode uses the match config `map_sides` (`team1_ct`/`team2_ct`) from the veto. `map_sides: "knife"` is refused at match load with a clear error unless `allow_knife: true` (TO option). The veto itself belongs to the platform. |
| **Coach** (L557-564): LAN only, may talk to players in warmup, half-time and tactical timeouts; online matches: no team staff present "physically or virtually" | coach role → whitelisted spectator (`fleet_state.cpp:320-321`) | Match config `lan: true\|false`. Online (`false`): coaches are **not** admitted to the server in esports mode. LAN: admitted as spectator; a coach cannot trigger a timeout by chat (L562: coach asks an admin). Voice rules are C. |
| **Demos** recorded and delivered (L356-357) | per-map `tv_record` + upload (`demo_recorder.cpp`) | Already met. Esports mode refuses to go live if GOTV is not up (`tv_enable` was 0 at map load), instead of silently skipping the demo. |
| **`tv_delay`** (Valve: any value) | not set; observed for flush timing | Match config `tv_delay` (default `105`, the Premier value) applied in `esports_live.cfg`; RU logs the effective value at go-live and blocks `tv_delay` lowering while live (console observer already exists in `match_plugin.cpp:294`). |
| **CSTV broadcast / caster camera** `tv_broadcast 1`, `tv_allow_camera_man_steamid` (L371, L417) | none | Match config `tv_broadcast_url`, `camera_man_steamid`; emitted as cvars before the map loads. Major-only broadcast upload stays a TO option. |
| **Overtime** `mp_overtime_limit 0` | `overtimeMode`/`maxOvertimes`/`damageTiebreak` | Esports mode ignores those three keys (warn at load) and never ends a map on damage. |
| **Substitutions**: one registered sub, swaps between matches (mid-match only for medical) (L542-549) | `sub` role exists | Roster changes accepted only while no map is live; mid-match `add_player` needs `admin_override: true`. |
| **Server mods**: only eBot allowed without Valve's written approval (L352-354) | – | C for Ready Up itself (see below); esports mode loads only `match` (+ `fleet`), never `skins`/`hello`. |

### C: not enforceable server-side

| Rule (source) | Owner |
|---|---|
| Ready Up is a server mod: at a **Major**, the TO needs Valve's written approval (L352-354) | TO |
| Client convars `cl_invites_only_mainmenu 1`, `cl_invites_only_friends 1` (L343-349) | TO / player PCs |
| No modified game art/sound; AR rules; workshop maps ≥100k subs, admin-downloaded (L279-290) | TO |
| Network/PC lockdown, SSDs, AppLocker, auditing (L292-341) | TO |
| Coach communication windows; staff in competition area; no staff online (L557-564) | admins |
| Registration, rosters, nicknames, VAC eligibility, CoI (L68-141) | TO / platform |
| Map pool (Active Duty, up-to-date versions) (L276-277) | platform veto config |
| Swiss/bracket format, seeding, Buchholz (L143-271) | platform |
| Major match logs: `logaddress_add_http` + `logaddress_token_secret` to Valve's Control Room, "SAVE FINAL SCORE" (L566-587) | TO (Ready Up can pass them through match `cvars`) |
| Binds/scripts | not covered by Valve's rulebook; TO rule |

## Conflicts with Ready Up's current defaults

1. **Base config.** `live.cfg` is a MatchZy-synced copy of old Premier values; it drifts (freezetime
   18 vs 20, timeout 30 vs 31, zeus 1 vs 5, OT money 10000). Esports mode execs Valve's files instead.
2. **Warmup.** Valve sets `mp_warmuptime 60` + `mp_warmup_pausetimer 1`; Ready Up turns CS2 warmup
   off and emulates it because the CS2 WARMUP banner hides the ready HUD. Keep the emulated warmup
   (warmup cvars are pre-match and Valve lets the TO choose `mp_do_warmup_period`), but set
   Valve's two values in `esports_live.cfg` so the live state is exact. Record this as a known
   deviation.
3. **Knife round.** Valve has no knife round; sides come from the veto. Scrims keep knives;
   esports matches refuse `map_sides: "knife"` unless `allow_knife: true`.
4. **Overtime.** Unlimited MR3 OT; Ready Up's `maxOvertimes` / damage tiebreak must not run.
5. **Pauses.** Unlimited `mp_pause_match` for everyone vs Valve's counted timeouts.
6. **Skins plugin.** Incompatible with L290; must be inert in esports mode.
7. **Coaches online.** Ready Up admits coaches as spectators; Valve forbids staff presence online.
8. **`mp_spectators_max`** 20 → 10.
9. **`bot_quota 1`** in Premier's base must be reset to 0 after the exec.

## Proposed switch

`readyup.cfg` (core key, above the first section, so every plugin can read it with `config_get`):

```ini
# Ruleset for loaded matches: default (MatchZy-style cfgs) | valve (docs/ESPORTS-MODE.md).
# A match config's "ruleset" wins over this.
ruleset=default
```

Match config (JSON, next to `map_sides`/`cvars`):

```jsonc
{
  "matchid": 123,
  "ruleset": "valve",                // "default" | "valve"
  "lan": false,                      // valve: false = coaches not admitted
  "map_sides": ["team1_ct", "team2_ct", "team1_ct"],   // from the veto; "knife" refused
  "allow_knife": false,              // TO option
  "cosmetics": "inventory",          // "inventory" (Valve) | "default_agents" (TO option)
  "tv_delay": 105,                   // Valve: any value
  "tv_broadcast_url": "",            // optional
  "camera_man_steamid": "",          // optional
  "cvars": { "mp_teamname_1": "...", "logaddress_add_http": "..." }  // still applied last
}
```

Effects of `ruleset=valve`: warmup uses `warmup.cfg` (unchanged); knife uses `knife.cfg` only if
`allow_knife`; go-live execs `ReadyUp/esports_live.cfg` instead of `ReadyUp/live.cfg`, then match
`cvars`; `ReadyUp/esports_override.cfg` is the per-server hook (like `live_override.cfg`). The
`ruleset` is published in `MatchState` (FLEET.md) so the platform can show it.

## `cfg/ReadyUp/esports_live.cfg`

```cfg
// Ready Up esports ruleset (ruleset=valve). Valve CS Major supplemental rulebook,
// counter-strike_rules_and_regs@8594dc0, "Server Settings > Convars".
// 1) Premier defaults, exactly as Valve ships them.
exec gamemode_competitive
exec gamemode_competitive_tmm

// 2) Valve's required exceptions (rulebook L362-418).
tv_enable                                   1
tv_enable1                                  1
tv_dispatchmode                             0
sv_maxusrcmdprocessticks                    3
sv_max_dropped_packets_to_process           3
tv_broadcast                                1   // written by RU only when tv_broadcast_url is set, else 0
tv_allow_autorecording_index                0
sv_steamauth_enforce                        1
mp_autokick                                 0
sv_matchend_drops_enabled                   0
sv_damage_print_enable                      0
sv_reliableavatardata                       2
sv_invites_only_mainmenu                    1
mp_spectators_max                           10
sv_matchpause_auto_5v5                      1
sv_clockcorrection_msecs                    0
mp_overtime_limit                           0
mp_warmup_pausetimer                        1
mp_warmuptime                               60
mp_halftime_pausematch                      1
mp_competitive_endofmatch_extra_time        155
mp_warmuptime_all_players_connected         0
sv_spec_hear                                4
tv_relayradio                               0
sv_vote_allow_spectators                    1
sv_vote_issue_loadbackup_spec_only          1
sv_vote_issue_loadbackup_spec_authoritative 1
sv_vote_issue_pause_match_spec_only         1
sv_vote_creation_timer                      25
sv_vote_failure_timer                       25
sv_occlude_players                          0
sv_force_transmit_players                   1
sv_force_transmit_ents                      1
sv_holiday_mode                             0
cash_team_bonus_shorthanded                 0
log                                         on
mp_logdetail                                3
mp_logdetail_items                          1
mp_logmoney                                 2
sv_gameinstructor_disable                   1
// game_type 0 / game_mode 1: launch args (take effect on map load); RU checks them at go-live.
// tv_allow_camera_man_steamid: from match config camera_man_steamid.

// 3) Not Valve rules; needed for a dedicated match server.
bot_quota                                   0
bot_kick
mp_autoteambalance                          0
mp_limitteams                               0

// 4) TO choices Valve allows (rulebook L421-476). tv_delay comes from the match config.
mp_disconnect_kills_players                 0
mp_halftime_pausetimer                      0
mp_overtime_halftime_pausetimer             0
spec_replay_enable                          0
sv_voiceenable                              1

// 5) Ready Up match management (no gameplay effect).
mp_backup_round_auto                        1
mp_backup_restore_load_autopause            1
mp_match_end_restart                        0
mp_team_intro_time                          0
mp_warmup_end

exec ReadyUp/esports_override.cfg
```

`gamemode_competitive.cfg` also sets `mp_warmuptime 120` and turns off nothing Ready Up needs;
the exceptions above run after it, so Valve's values win.

## Implementation order (for the follow-up PR)

1. `ruleset` key (core `readyup.cfg` + match config), `esports_live.cfg`, go-live cfg switch,
   `MatchState.ruleset`. (S)
2. Refuse knife / OT keys / online coaches under `valve`; `tv_delay` + camera-man cvars. (S)
3. Skins plugin inert under `valve`. (S)
4. Pause rework: tactical via `timeout_*_start`, counted technical, halftime + auto-5v5 pause
   types. Shares work with PARITY.md §7 P1. (M)
5. Optional `cosmetics: "default_agents"` + `esports_default_agents.cfg`. (M, behind a flag)
6. Livetest: `scripts/livetest --ruleset valve` asserts every exception cvar with `cvarlist`
   after go-live, so a Valve rulebook or CS2 update that renames a cvar fails CI.
