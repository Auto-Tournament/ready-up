# Ready Up architecture: core + plugins

Ready Up is a fake `libserver.so`. `gameinfo.gi` lists `Game csgo/readyup` first, so the
engine loads our shim, which `dlopen`s Valve's real `libserver.so` and forwards its exports.
Up to now everything (engine plumbing, match flow, skins) was one monolithic shim. This
document describes the split:

| Piece | Ships as | Owns |
|---|---|---|
| **readyup-core** | `csgo/readyup/bin/linuxsteamrt64/libserver.so` | every piece of engine surface, plus the plugin host |
| **readyup-match** | `csgo/readyup/plugins/match.so` | ready-up, scrim, knife, pause, match config, webhooks, Postgres, MAT admins, practice |
| **readyup-skins** | `csgo/readyup/plugins/skins.so` | weapon paints, knives, gloves, agents. **Not in the default release** (servers running skin changers risk GSLT bans) |
| **readyup-fleet** | `csgo/readyup/plugins/fleet.so` | the Auto Tournament platform link ([FLEET.md](FLEET.md)): enrollment, WebSocket, spool; `readyup.fleet.v1` for other plugins |
| example: **hello** | `csgo/readyup/plugins/hello.so` | `.hello`, a 10 s tick heartbeat, event logging. Dev only, never shipped |

The model is Metamod's. The core is the only thing that knows about signatures, offsets,
vtable indices, RTTI, hooks and SDK struct layouts. Plugins only call a versioned table of C
function pointers. The goals:

1. **CS2 updates touch the core only.** When Valve moves a function, `gamedata/engine-surface.json`
   and core code change. Plugins built against the same API version keep working without a rebuild.
2. **Hot reload.** `ru plugin reload match` swaps the match logic without restarting the server.
   This is the main speed-up for development.
3. **Separate shipping.** Skins is its own `.so`. Leaving it out of a release means removing one file.

Status: the plugin host, API v1.2, `plugins/hello` and `plugins/skins` work (steps 0-3 of the
[migration plan](#migration-plan)). Match is **still compiled into the core** and behaves exactly
as before; moving it out is step 4 (API v1.2 added what it needs).

---

## 1. The boundary

The rule: **if it depends on a byte pattern, an offset, a vtable index, a struct layout, a
hook or an engine interface, it is core.** Everything else is policy and belongs in a plugin.

The core owns:

- **Loading.** The `libserver.so` shim, forwarding exports and `CreateInterface`, `real_server`.
- **Engine surface.** Resolving `engine-surface.json` (signatures + identity anchors), sigtest,
  fail-closed "disabled" mode, and `readyup_sigcheck`.
- **Hooks** (funchook detours / vtable / GOT patches): GameFrame, Host_Say, ClientCommand,
  CCommandBuffer::AddText, TerminateRound, the logging listener.
- **RTTI-verified interfaces**: the game event manager, the legacy per-client listener, the entity system.
- **Game events**: registering the listener and turning events into normalized lifecycle events.
- **Threads and commands**: the GameFrame thread queue (`post_to_game_thread`, per-tick
  callbacks) and server command enqueue.
- **Command routing**: chat and console commands (dedupe across the three chat paths; core
  commands reserved).
- **Logs**: the log-line listener and log-derived lifecycle events (the fallback when engine events are down).
- **Schema and entities**: field offsets by name, entity lookup and iteration, and entity
  primitives (attribute by name, subclass, model, bodygroup, network-state-changed).
- **Output**: chat to all or to one slot (`UTIL_ClientPrintAll` / `ClientPrint`), center HTML (broadcast or per client).
- **Players**: the identity and team registry (slot ↔ SteamID64 ↔ name ↔ team, bots).
- **Infrastructure**: config (`readyup.cfg` core keys, plus per-plugin config lookup), logging, the crash handler.
- **Admin checks**: the permission check (`is_admin`). Who counts as an admin comes from a
  provider; the match plugin registers the MAT/DB-backed one.

Plugins own **policy**. They decide what happens on a round start, what `.r` means, which
webhook to send and which skin to apply. They get engine access only through `ru_api`.

## 2. The C ABI (`core/include/readyup/plugin_api.h`)

That header is the whole contract. Plugins include it and nothing from the core's sources.

### What a plugin exports

```c
READYUP_PLUGIN_EXPORT const ru_plugin_info* readyup_plugin_info(void);
READYUP_PLUGIN_EXPORT int  readyup_plugin_load(const ru_api* api, uint32_t core_api_version);
READYUP_PLUGIN_EXPORT void readyup_plugin_unload(void);
```

- `readyup_plugin_info` is called right after `dlopen` and before anything else. It returns
  `{struct_size, api_version, name, version, author, description}`. `name` must equal the
  file name (`hello` ↔ `hello.so`). The core rejects the plugin on an API major mismatch, or
  if the plugin needs a newer minor than the core has.
- `readyup_plugin_load` runs on the game thread. The plugin registers its commands, ticks and
  subscriptions here and keeps `api`, which stays valid until unload returns. Returning
  non-zero aborts the load: the core removes anything already registered and `dlclose`s.
- `readyup_plugin_unload` runs on the game thread at a safe point (see §4).

### What the core gives it (v1.0, implemented)

Every function takes `api->self` (the plugin's `ru_plugin*`) first. The core uses it to
attribute every registration to its owner.

| Member | Thread | Purpose |
|---|---|---|
| `struct_size`, `api_version`, `core_version`, `self` | – | versioning and identity |
| `log(self, level, msg)` | any | console log line `[ReadyUp] plugin[name]: ...` (`ru_logf` is a printf helper in the header) |
| `server_command(self, cmd)` | game | queue a server console command (the core's `EnqueueServerCommand`) |
| `chat_all(self, msg, flags)` | game | chat to everyone, with the Ready Up prefix (or `RU_CHAT_RAW`) |
| `chat_to_slot(self, slot, msg)` | game | chat to one player (`ClientPrint`) |
| `register_chat_command(self, ".name", fn, user)` | game | chat command. Core commands (`.ru`, `.r`, `.pause`, …) are reserved. Only real players (SteamID ≠ 0) trigger it |
| `register_console_command(self, "name", fn, user)` | game | server console / RCON command. `ru` and every core command win over plugin commands |
| `on_tick(self, fn, user)` | game | once per simulating GameFrame, after the core's frame work |
| `subscribe(self, type, fn, user)` | game | lifecycle events (below). `RU_EVENT_ANY` for all |
| `unregister(self, handle)` | game | remove any of the above (safe from inside that callback) |
| `post_to_game_thread(self, fn, user)` | **any** | hand work from a plugin's worker thread back to the game thread. Dropped if the plugin unloads first |
| `slot_for_steamid(self, steamid64)` | game | engine slot of a connected player, or -1 |
| `data_dir(self)` | game | `csgo/readyup/plugins/<name>/` for plugin data and config |

**Lifecycle events** (`ru_event`) arrive already normalized. They come from engine game
events once the core's listener is live, and from server log lines until then (`source` says
which). A plugin never has to reconcile the two sources.

| Event | Fields |
|---|---|
| `RU_EVENT_MAP_START` | `map` |
| `RU_EVENT_MATCH_START` | CS2 `Match_Start` (warmup end, `mp_restartgame`) |
| `RU_EVENT_ROUND_START` | `round` |
| `RU_EVENT_ROUND_END` | `winner` (2 = T, 3 = CT), `reason`, `round`, scores when known |
| `RU_EVENT_PLAYER_CONNECT` / `DISCONNECT` | `slot`, `steamid64`, `name` (+`reason`) |
| `RU_EVENT_PLAYER_TEAM` | `slot`, `steamid64`, `name`, `team`, `old_team` |

Every event also carries `map` (the current map).

### v1.1 (implemented)

Appended to `ru_api` in this order; `plugin_api.h` has the exact signatures and comments.
Bookkeeping members live in `plugin_loader.cpp`; engine-facing ones in `plugin_engine_api.cpp`.
Everything is game-thread only except where noted.

| Group | Members | Notes |
|---|---|---|
| Output | `center_html_to_slot`, `center_html_all` | per-client center HTML (never a broadcast event); `_all` loops over connected humans |
| Players | `get_player`, `get_player_by_steamid`, `for_each_player` | from the core's human/bot registry; `ru_player` is caller-owned with an inline `name[128]` |
| Raw engine events | `subscribe_game_event(name)`, `ev_get_int/float/uint64/string/player_slot/player_controller/player_pawn` | **synchronous** on the game thread inside the engine's dispatch, before the core takes its events lock. The core adds its listener for every subscribed name (re-checked when the set changes and every 2 s) |
| Log lines | `subscribe_log_line` | queued copies; fed from `ObserveLifecycleLogLine`, which both the in-process listener and the file-tail fallback go through |
| Schema / entities | `schema_offset`, `entity_system_status`, `entity_by_index`, `entity_from_handle`, `entity_handle_of`, `entity_classname`, `entity_mark_changed`, `econ_attr_set_by_name`, `entity_change_subclass`, `entity_set_model`, `entity_set_bodygroup_by_name` | backed by `schema.*` and `entity.*`; each returns 0/NULL when its engine function did not resolve |
| Match control | `set_round_termination_suppressed`, `set_chat_name_prefix` | see below for `terminate_round` |
| Admins | `is_admin` (**any thread**, may block), `set_admin_provider` | the core's `IsReadyUpAdmin` asks the provider first, so core checks follow it too. Providers run on the caller's thread under a shared lock that unload takes exclusively |
| Config | `config_get`, `debug_enabled` (any thread), `config_dir` (any thread) | `config_get` reads `csgo/cfg/ReadyUp/<plugin>.cfg` (top-level keys or a `[<plugin>]` section), then the `[<plugin>]` section of `readyup.cfg`. The core's own parser now ignores everything after the first `[section]` line |
| Plugin-to-plugin | `provide_interface`, `get_interface` | one provider per name, removed on unload; look it up again in each callback |
| Reload state | `stash_put`, `stash_get` | byte blobs keyed by (plugin, key), kept in core memory across unload/load, max 1 MiB each |

Deviations from the original plan:

- **`terminate_round` is not in v1.1.** The core only has the TerminateRound *suppression*
  detour; calling TerminateRound needs the CCSGameRules pointer and a verified ABI, which the
  engine surface does not have. `set_round_termination_suppressed` covers what match uses today.
- **`set_chat_name_prefix`** is keyed by SteamID64 and feeds the existing relay (the core
  re-sends `"<prefix> <name>: <msg>"` and swallows the original), not a real name swap.
- **`RU_CMD_HIDE` / `register_chat_command_ex`** and `RouteChatCommand` carrying the sender slot
  were not in v1.1; they are in v1.2 (below), ahead of the match move.

`plugins/hello` requires 1.0 and uses every 1.1 group it can behind `RU_API_HAS` (stash load
counter, `greeting` from config, `player_death`, log-line count, `readyup.hello.v1`), and the
host test checks each of them, including that nothing is delivered after unload.

### v1.2 (implemented)

What moving the match flow out of the core needed. Appended to `ru_api` in this order (plus two
appended struct fields); bookkeeping members in `plugin_loader.cpp`, `feature_state` in
`plugin_engine_api.cpp`.

| Member | Thread | Purpose |
|---|---|---|
| `log_untagged(self, level, msg)` | any | `[ReadyUp] <msg>` without the `plugin[name]: ` tag, for log formats tools already parse (`state:`, `knife:`) |
| `register_chat_command_ex(self, name, flags, fn, user)` | game | chat command with `RU_CMD_HIDE`: the core swallows the sender's line in its ClientCommand hook, before the engine prints it |
| `register_console_command_ex(self, name, flags, fn, user)` | game | console command with `RU_CMD_OBSERVE`: the plugin sees the line, the engine still runs it (e.g. `tv_delay 5`). Observers never conflict |
| `register_ru_subcommand(self, name, fn, user)` | game | `ru <name> ...` (console / RCON) and `.ru <name> ...` (chat) both reach fn; argv[0] is `ru` / `.ru`. The core's subcommands (`help`, `plugin`, `version`, `selftest`, `sigtest`, `reload`, `status_http`) are reserved |
| `on_frame(self, fn, user)` | game | every GameFrame, simulating or not (`ru_tick_info.simulating`), after `on_tick`. For timers that must fire while the server does not simulate |
| `feature_state(self, name)` | game | 1 on / 0 pending / -1 off for a core feature (`knife`, `ready_hud`, ...) or dependency (`fn:X`, `cmdbuf`, `loglistener`, `eventmgr`, ...), plus `events_live` (engine events drive the round lifecycle) |
| `current_map(self)` | game | the map of the last `RU_EVENT_MAP_START`, e.g. for a plugin that loads mid-map |

Struct fields: `ru_tick_info.simulating`, `ru_player.userid` (the log `<N>`, what `kickid` takes).

**Sender slot.** Chat commands now carry the sender's slot from the ClientCommand hook (it used
to be looked up again from the SteamID when the command was delivered). The hook routes every
plugin-owned chat command, not only `.ru` / `.r`; the log listener's copy of the line is deduped.

**Still not in the API: `terminate_round`.** Calling `CCSGameRules::TerminateRound` needs the
game rules pointer and a verified calling convention for the float/reason arguments; neither is
in the engine surface, and a wrong guess crashes the server mid-match. Match keeps using
`set_round_termination_suppressed` (the detour that returns early), which is all it needs.

Plugin lines in `ru selftest` are not an API member: a plugin publishes the
`readyup.selftest.<name>` interface (`core/include/readyup/selftest_iface.h`).

`plugins/hello` (still requiring 1.0) now also registers `ru hello`, a hidden `.hellohide`, an
`sv_cheats` observer and an `on_frame` counter; the host test checks each one, including that
they disappear on unload.

### ABI rules

1. **Plain C across the boundary.** No C++ classes, references, STL, `std::string` or
   exceptions. Plugins may use C++ inside but must catch everything before returning into the
   core. The core also catches anything thrown out of a callback and logs it, but a throw
   across the C boundary is a plugin bug.
2. **Fixed-width scalars** (`int`, `uint32_t`, `uint64_t`, `double`) and pointers only. Enum
   values travel as `uint32_t`; `int` is used as the boolean.
3. **No ownership transfer.** Strings and structs passed to a callback live only for that call.
   The core never frees plugin memory and plugins never free core memory. `user` pointers are opaque.
4. **Only `extern "C"` symbols cross.** Plugins export exactly the three entry points (hidden
   visibility for everything else) and import nothing from the core. They link with
   `--no-undefined`, and every core service is a function pointer in `ru_api`.
5. **`-fno-gnu-unique`** for C++ plugins (set by `readyup_add_plugin`). `STB_GNU_UNIQUE`
   symbols make `dlclose` a no-op, and a reload would then silently keep the old code. The
   core checks with `RTLD_NOLOAD` after `dlclose` and warns if the image is still mapped.
6. **Versioning.** `READYUP_PLUGIN_API_VERSION = (MAJOR << 16) | MINOR`.
   - **MINOR**: new members are appended to `ru_api` (or to a callback struct). Nothing is
     removed, reordered or changed in meaning. A plugin built for 1.0 runs on a 1.3 core. A
     plugin that wants a 1.2 member either requires 1.2 in `ru_plugin_info.api_version`
     (older cores refuse it) or checks `RU_API_HAS(api, member)` at runtime and degrades.
   - **MAJOR**: any incompatible change. The core refuses plugins with another major. Bump it
     rarely, and only together with the plugins in this repo.
   - Every struct begins with `uint32_t struct_size`. Readers check it before touching
     trailing fields.
7. **Compatibility policy.** A core release supports its own API major, and every minor up to
   its own. Plugins in this monorepo are always built with the core they ship with.
   Third-party plugins only need to be rebuilt on a major bump.

## 3. Threading rules

- **Every plugin callback runs on the server main thread**, the one that runs
  `ISource2Server::GameFrame`: load, unload, commands, events, ticks and posted tasks.
- **API functions** must be called from that thread (they are, when called from a callback).
  Off-thread calls are rejected and logged. The exceptions are `log` and `post_to_game_thread`,
  which are thread-safe.
- **Chat, console and event producers may run on any thread** (log listener, AddText caller,
  file tail thread). The core only **queues** there; delivery happens in the next GameFrame.
  As a result plugin code never runs inside the AddText detour or (except for raw
  `subscribe_game_event` callbacks, which are synchronous by design) the engine's event dispatch,
  and never while a core mutex is held (no lock-order problems between core and plugin
  locks). The cost is at most one frame of latency (about 15 ms at 64 tick).
- **Frame order**: original GameFrame → core ticks (modes, scrim, welcome, skins, for now) →
  `plugins::Frame`: pending load/unload/reload, then posted tasks, commands and events (FIFO
  per queue), then `on_tick` if simulating. Queues and ops also drain on non-simulating frames.
- **Worker threads** (DB, HTTP) belong to the plugin. It starts them in load, **joins them in
  unload**, and hands results back with `post_to_game_thread`.
- Queues are bounded (1024 commands / events, 4096 tasks). Floods drop the oldest events and
  refuse new commands and tasks instead of growing without limit.

## 4. Hot reload and its safety

`ru plugin list | load <name> | unload <name> | reload <name>` works from the server console,
RCON, and chat (`.ru plugin ...`, admins only). `reload` = unload + load of
`csgo/readyup/plugins/<name>.so` from disk.

**The op is deferred to a safe point.** The command only queues the request. It runs at the
top of `plugins::Frame` in the next GameFrame. At that point no plugin code is on the stack
(`g_depth == 0`), the engine is not inside AddText or event dispatch, and the core holds no
lock. A plugin that runs `server_command("ru plugin reload x")` therefore cannot unload
itself mid-callback.

**Unload sequence:**

1. Mark the plugin `unloading`. The core stops dispatching to it and refuses its `post_to_game_thread`.
2. Call `readyup_plugin_unload()`. The plugin joins its threads and frees its memory.
3. The core removes **every** registration the plugin owns (chat and console commands, ticks,
   subscriptions) and every queued task it posted, whether or not the plugin unregistered them itself.
4. Mark the handle dead, then `dlclose`, then check with `dlopen(RTLD_NOLOAD)` that the image
   is really gone (and warn if not).

**What that guarantees:**

- **No dangling hooks.** Plugins cannot hook anything. Every detour belongs to the core and
  outlives any plugin, so there is never a trampoline into an unmapped image.
- **No stale callbacks.** Queued commands and events are resolved by registration handle when
  they are delivered. Anything queued for the old image before the reload is dropped. It is
  never delivered to the new one, and never into unmapped code. (The host test checks this.)
- **No use-after-free on stale handles.** `ru_plugin` and the `ru_api` table are never freed
  (a few hundred bytes per reload). A plugin thread that wrongly outlives unload finds
  `alive == false`, and its calls become no-ops instead of touching freed memory. Its own
  code, however, is unmapped, which is why joining threads in unload is the one rule the core
  cannot enforce.
- **Failed loads leave nothing behind**: bad exports, version mismatch, name mismatch, or
  `load` returning non-zero.
- **Crash attribution.** While a plugin callback runs, the crash handler prints
  `crashed inside plugin "<name>"` before the backtrace.
- **State is not carried over** unless the plugin uses `stash_put`/`stash_get`. If the
  new file fails to load, the plugin stays unloaded. `dev-deploy.sh` keeps `<name>.so.prev`,
  so `mv` it back and reload.

**Skins-specific cleanup** (for when it becomes a plugin):
- Stop and join the loadout-fetch DB threads. Drop the per-player loadout cache and the
  "decorate next frame" lists.
- Never keep `CEntityInstance*` across frames. Keep entity handles and re-resolve them with
  `entity_from_handle`, so nothing points at entities freed while the plugin was unloaded.
- Paint, knife, glove and agent changes already applied stay on live entities. They are
  ordinary entity state and reset on respawn or map change. Unload does not try to revert them.
- The econ and entity primitives stay in the core. The skins signatures should move to a
  gamedata fragment (`engine-surface.skins.json`) that ships **only in the skins zip**. The
  default core then does not even resolve them, and the matching `ru_api` members return 0.

**Match-specific:** a reload mid-match drops in-memory match state. The plugin must restore
from `persisted_match_state` (the crash-recovery path it already has) or from `stash_*`.
During development, reload between matches.

**Hibernation:** reload runs in GameFrame. If an empty server stops producing frames, a
queued reload waits until it wakes. Dev servers should run with `sv_hibernate_when_empty 0`.

## 5. Where the current modules go

The physical move (step 1) is done: core sources are in `core/src/readyup/` (the
`readyup/` subdirectory keeps every `#include "readyup/..."` unchanged), the shim entry
points in `core/src/exports.{cpp,map}`, vendored code in `third_party/`, and the non-engine
helpers in `libs/readyup/`. Match and skins sources still sit in `core/src/readyup/` and are
still linked into the core until steps 3 and 4 move them.

### Core (`core/src/readyup/`)

| Files | Notes |
|---|---|
| `exports.cpp`, `real_server.*`, `path.*`, `disabled.*`, `banner.*`, `version.h`, `cs2_version.*` | shim, loading |
| `engine_surface.*`, `engine_surface_core.*`, `signature_scan.*`, `sigtest.*`, `sdk/*` | engine surface |
| `game_frame_hook.*`, `host_say_hook.*`, `client_command_hook.*`, `round_termination_hook.*`, `console_command.*` | hooks (`console_command` is a probe and can go) |
| `server_game_clients_hook.*` | **split**: the ClientCommand hook + name-swap primitive stay; the admin/captain prefix *policy* goes to match |
| `command_buffer_hook.*` | **split**: the AddText hook, `EnqueueServerCommand` and core `ru` subcommands (`help`, `plugin`, `sigtest`, `reload`, `mode` forwarding) stay; `ru match load`, token/url/warmup cvar commands go to match as console commands |
| `game_events.*` | **split**: listener install, RTTI, registration, normalized events, slot→controller map and team-for-slot stay; round stats, damage, halftime/OT, knife and webhook emission go to match |
| `log_receiver.*` | **split**: the file-tail fallback and line fan-out stay; `LifecycleImpl`'s webhook/score/backup logic goes to match |
| `chat.*`, `chat_colors.h`, `client_print.*`, `center_html.*` | output |
| `schema.*`, `entity.*` (was `skins_engine.*`) | schema and entity primitives (the generic half of skins) |
| `slot_registry.*`, `player_registry.*`, `steamid.*` | identity and team registry |
| `ru_router.*`, `ru_help.*` | **split**: routing, dedupe, `.ru` / `.ru plugin` / version stay; every match command (`.r`, `.pause`, `.ru start`, …) becomes a match registration |
| `config.*` | core keys stay (debug, banner, prefixes, log port); match keys move behind `config_get` |
| `logging.*`, `crash_handler.*`, `minijson.*` | infrastructure |
| `plugin_loader.*` (new) | plugin host |

### readyup-match (`plugins/match/`)

`modes.*`, `scrim_flow.*`, `match_state.*`, `pause_state.*`, `match_config_parser.*`,
`match_recovery.*`, `match_token.*`, `persisted_settings.*`, `persisted_match_state.*`,
`backup_files.*`, `webhook.*`, `welcome.*`, `admins.*`, `admin_check.*` (it becomes the admin
provider), `mat_admins.*`, plus the match halves of the split files above.

### readyup-skins (`plugins/skins/`)

**Done (step 3).** `skins_plugin.cpp` (entry points, `skins_status` / `skins_refresh` /
`skins_debug_as` console commands), `loadout.cpp` (was `weapon_paints.cpp` plus the
`readyup_weapon_*` queries that used to live in the core's `postgres.cpp`), `apply.cpp`,
`cosmetics.cpp`, `apply_internal.h`, `stattrak.cpp` (the `player_death` StatTrak bump that used
to sit in `game_events.cpp`), `legacy_paint_kits.inc` + `gen_legacy_paint_kits.py`,
`seed-dev-skins.sql`, `docs/db-contract.md`, `docs/engine-surface.md`. Gamedata:
`gamedata/engine-surface.skins.json` (below).

How it talks to the core, all through `ru_api` v1.1:

- per tick: `on_tick` drives the same controller/weapon walk as before (entity_by_index,
  entity_from_handle, entity_classname, schema_offset);
- paints / knives / gloves / agents: econ_attr_set_by_name, entity_change_subclass,
  entity_set_model, entity_set_bodygroup_by_name, entity_mark_changed;
- prefetch: raw `player_spawn`, `item_equip`, `item_pickup` (the core no longer listens to the
  last two); StatTrak: raw `player_death`;
- DB: its own libpq connection (`libs/readyup/pg_client`) configured from
  `config_dir()/readyup_db.json` (`libs/readyup/db_config`), on one worker thread that unload joins.

**Gamedata fragment.** The engine entries only skins uses (the econ/model functions and the
`CEntityInstance::NetworkStateChanged` slot) moved from `engine-surface.json` to
`gamedata/engine-surface.skins.json`. The core merges every `engine-surface.<name>.json` next to
the shim at load (entries may only be added, never overridden); the skins packages ship the
fragment, the others do not. `readyup_sigcheck` / `readyup_hookcheck` take fragments as extra
arguments and `scripts/ci/verify-cs2.sh` passes every fragment in `gamedata/`.
`dev-deploy.sh --plugin skins` installs the fragment too (a new fragment needs `--restart`).

### Shared non-engine code (`libs/`)

`postgres.*`, `db_config.*`, `http_client.*`, `minijson.*` and `steamid.*` become small static
PIC libraries. Step 3 made a start: `readyup_libs_db` (`db_config` parsing + `pg_client`, no core
dependencies) is what skins links; the core's `postgres.*` (admins, settings) goes with match. Each plugin links in what it needs, statically and with hidden visibility.
This is source-level sharing: nothing crosses the ABI, and none of it touches the engine. The
core keeps only `minijson`/`steamid` and, after the move, needs neither libpq nor libcurl.

### Target monorepo layout

```
core/                     libserver.so shim: engine surface, hooks, events, loader, API impl
core/include/readyup/plugin_api.h   public C ABI (plugins include only this)
plugins/match/            readyup-match
plugins/skins/            readyup-skins (separately shippable)
plugins/hello/            example plugin
libs/                     shared non-engine code (postgres, http, json) as static libs
gamedata/                 engine-surface.json (+ engine-surface.skins.json)
third_party/              funchook, distorm (licences kept)
tools/                    sigcheck, plugin host test, generators
scripts/ docs/ cfg/ .github/
```

The top-level CMake builds everything. Each plugin is its own target
(`readyup_add_plugin(<name> ...)` → `readyup_plugin_<name>` → `build/plugins/<name>.so`), so
`cmake --build build --target readyup_plugin_match` rebuilds one plugin without relinking the core.

## 6. Build, release and dev workflow

**Build:**

```bash
./build.sh                                  # build/libserver.so, build/plugins/hello.so, tests
(cd build && ctest --output-on-failure)     # offline plugin host test (load, commands, events, hot reload)
scripts/docker-build.sh                     # Debian 12 image, static libpq -> build-docker/
BUILD_TARGET=readyup_plugin_hello scripts/docker-build.sh   # one plugin only
build/readyup_sigcheck ~/ru-analysis/libserver.so gamedata/engine-surface.json
```

`tools/plugin_host_test.cpp` links the real `plugin_loader.cpp` against stubs of the few core
functions it calls. It loads `hello.so`, dispatches chat and console commands and events,
ticks, then hot-swaps in a second build of hello (`build/test/hello.so`, version
`1.0.1-reloaded`), reloads it and checks that the new code runs, the old image is unmapped,
and stale queued commands are dropped.

**Installed layout:**

```
csgo/readyup/bin/linuxsteamrt64/libserver.so      core
csgo/readyup/bin/linuxsteamrt64/engine-surface.json
csgo/readyup/plugins/match.so                     every release
csgo/readyup/plugins/skins.so                     skins release only
csgo/readyup/plugins/<name>/                      plugin data dir (optional)
```

Every `*.so` in `plugins/` loads on the first server frame, in name order. To disable one,
rename it (for example `skins.so.off`) or `ru plugin unload` it. `READYUP_PLUGINS=0` disables
all of them. `READYUP_PLUGINS_DIR` overrides the directory (tests).

**Release** (`scripts/package-release.sh`, built and uploaded by CI on every run):

- component zips `ready-up-{core,match,skins,hello}-X.Y.Z-linuxsteamrt64.zip` (match is a
  manifest-only placeholder until step 4), which the root `install.sh` mixes;
- bundles `ready-up-essentials-...` (core + match, the default, no skins) and
  `ready-up-full-...` (core + match + skins + hello + the gamedata checkers);
- `SHA256SUMS`.

Every zip carries `readyup/manifests/<component>.json` (its file list). `scripts/ci/check-bundles.sh`
fails CI if core, match or essentials contain skins code or gamedata, and the `installer` job
runs `tests/installer/test_install.sh` against the built zips. Plugins carry the release version; the API version is separate.
CI runs sigcheck against the core only; plugins have nothing to check.

**Dev loop (no server restart):**

```bash
scripts/dev-deploy.sh --plugin match      # build only readyup_plugin_match, copy match.so to
                                          # server-4/game/csgo/readyup/plugins/, then send
                                          # `ru plugin reload match` into the ru-4 tmux console
                                          # as cs2servermanager and print the result
scripts/dev-deploy.sh --restart           # core changes still need the old full deploy + restart
```

`--plugin` renames the file into place (a running image is never overwritten), keeps
`<name>.so.prev`, and waits up to 15 s for `plugin: reloaded <name>` / `reload <name> failed`
in `console.log`.

## 7. Migration plan

Order matters. The big move must not land while `selftest`, `knife-hud`, `ci` and the parity
docs branches are still open, because every one of them touched `src/readyup/*` and `CMakeLists.txt`.

| Step | What | Effort |
|---|---|---|
| 0 | Plugin host, API v1.0, `hello`, host test, `dev-deploy --plugin` (this branch). Match and skins unchanged. | done |
| 1 | **Done.** One `git mv` commit to the target layout (`src/readyup` → `core/src/readyup`, `src/third_party` → `third_party`, `src/exports.*` → `core/src`, postgres/db_config/http_client/minijson/steamid → `libs/readyup`), with CMake, scripts, CI and docs paths updated. No code changes, so review is trivial and `git log --follow` keeps history. | 0.5 day |
| 2 | **Done.** API v1.1: the additions listed in §2 (players, center HTML, raw engine events, log lines, schema/entity/econ, terminate round, name prefix, admins, config, interfaces, stash) and `RouteChatCommand` carrying the sender slot (Host_Say already knows it). | 2–3 days |
| 3 | **Done.** Extract **skins** to `plugins/skins`: `libs/` for postgres/db_config, `skins_engine` → core `entity.*`, split out the skins gamedata fragment, replace `weapon_paints::GameFrameTick` / `MaybeRefreshAsync` call sites with `on_tick` and event subscriptions. Verify on server-4 (paints, knife, gloves, agents; reload mid-map). Release script: two zips. | 2–3 days |
| 4 | Extract **match**. This is the big one: `modes` (1.8k lines), `webhook`, the match half of `game_events` (~1k), `ru_router`, `command_buffer_hook` commands, `log_receiver` lifecycle, scrim, welcome and admins become plugin code using the v1.1 API. The core keeps the split halves. Then a full regression on server-4 with MAT: match load, ready, knife and side pick, pause/unpause, halftime/OT, recovery after restart, webhooks, scrim flow, practice. | 5–7 days |
| 5 | Cleanup: drop libpq/libcurl from the core link, document writing third-party plugins, and add the CI job that builds both zips and runs the host test. | 1 day |

**Total: roughly 2–2.5 weeks of focused work** after step 1 is unblocked. Steps 3 and 4 are
independent once step 2 lands, so skins can ship as a plugin before match moves.
