# Development

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Output:
- `build/libserver.so`
- `build/plugins/<name>.so` (plugins; see [ARCHITECTURE.md](ARCHITECTURE.md))

Plugins hot-reload without a server restart: `scripts/dev-deploy.sh --plugin <name>`.
The offline plugin host test runs with `(cd build && ctest --output-on-failure)`.

## Release build (Steam Runtime sniper)

Release artifacts are built in Valve's Steam Runtime 3 "sniper" SDK image (glibc 2.31),
the runtime CS2 targets, so the shim loads on any CS2 host:

```bash
scripts/sniper-build.sh          # -> build-sniper/libserver.so (docker)
```

- `scripts/ci/build-static-deps.sh` builds pinned, sha256-checked OpenSSL, libpq (client
  only) and libcurl as static PIC archives (cached in `~/.cache/readyup-sniper-deps`).
- CMake `-DREADYUP_DEPS_PREFIX=<prefix>` links them plus libstdc++/libgcc statically, uses
  GCC 14 from the SDK (sniper's GCC 10 libstdc++ rejects `unordered_map` with an incomplete
  value type), and pins the dynamic symbol table to `core/src/exports.map`.
- `scripts/ci/check-portable.sh` fails the build if the result needs anything but glibc,
  a symbol newer than GLIBC_2.31, or exports anything beyond `core/src/exports.map`.
- libcurl's CA bundle is probed at runtime (Debian, RHEL, SUSE, Alpine paths).
- libstdc++ is linked by hand ahead of `exports.cpp`, so `readyup_ctor` stays the last
  static constructor (checked by `scripts/check_shim_binary.sh`).

`scripts/docker-build.sh` (bookworm, dynamic OpenSSL/curl) is still fine for dev iteration
on the test box.

## CI

- `.github/workflows/build.yml` (push to master/dev/*, PRs, tags `v*`): sniper build, unit
  tests, then `readyup_sigcheck` + `readyup_hookcheck` against the **current public CS2
  build** (fetched as below, cached per buildid). Uploads the zip + tools as an artifact.
  Tags `v*` (must match `VERSION`) publish the GitHub release; `./release.sh` just bumps,
  tags and pushes.
- `.github/workflows/cs2-update-watch.yml` (every 15 min): polls CS2's public buildid via
  api.steamcmd.net; on a new build (or a changed engine surface) it downloads only
  `libserver.so`, `libengine2.so`, `libtier0.so`, `libschemasystem.so` (depot 2347773) and
  `steam.inf` (depot 2347770) anonymously with DepotDownloader, runs both checks, and
  opens/refreshes a `cs2-update` issue (+ optional Discord post via the
  `DISCORD_WEBHOOK_URL` secret) on failure, or comments "CS2 build N verified". State lives
  on the `cs2-build` branch.

Run the same verification locally:

```bash
scripts/ci/fetch-cs2-binaries.sh /tmp/cs2        # ~18 MB, anonymous
scripts/ci/verify-cs2.sh /tmp/cs2 build-sniper   # sigcheck + hookcheck + report.md
```

## Debug install

```bash
sudo ./install.sh --debug
```

This enables Ready Up debug logging via the generated `readyup.cfg`.

## Signatures (engine surface)

Every engine function Ready Up calls or hooks is listed in `gamedata/engine-surface.json`
together with **identity anchors** (strings the function, or its callers, must reference).
The file is embedded into `libserver.so` at build time; `install.sh` also copies it next to
the shim as `engine-surface.json`, which takes precedence (so a signature can be hotfixed
without a rebuild). The CounterStrikeSharp CDN gamedata is no longer used.

A function only resolves if its signature matches exactly once **and** every anchor passes.
Otherwise it stays unresolved and its feature turns itself off (or, for `"required": true`
entries, Ready Up disables itself at load).

Virtual slots we patch or call (`vtable_indices`: GameFrame, ClientCommand,
NetworkStateChanged) are verified too. The implementing class's vtable is found through its RTTI
typeinfo name (`rtti`). The slot's target must equal the named `function` and/or pass its
`anchors` (for example the `"GameFrame"` VPROF string, or `mov_disp` anchors that pin the CCommand
argc/argv offsets in `layouts`). At runtime the live interface object's vptr must also equal that
vtable. A slot that fails is never patched.

Every feature declares the engine-surface entries it needs (`core/src/readyup/features.cpp`). If one
is unresolved or unverified, the feature logs one `feature <name> DISABLED: needs ...` line and
turns itself off. Everything else keeps running.

After a CS2 update, check the new binary offline before deploying:

```text
build/readyup_sigcheck /path/to/game/csgo/bin/linuxsteamrt64/libserver.so gamedata/engine-surface.json
```

It checks functions, RTTI classes, vtable slots and layouts, and exits non-zero on any failure.

Hooks use vendored upstream funchook (`third_party/funchook`, diStorm backend), which
relocates the displaced prologue instructions instead of copying a fixed number of bytes.

Entries marked `"hook": "funchook"` are detoured. `build/readyup_hookcheck <libserver.so>
gamedata/engine-surface.json` resolves each one and runs `funchook_prepare` on a mapped copy
of Valve's binary (decode + relocate the prologue into a trampoline, no install), failing if
the prologue can't be relocated (e.g. a branch back into the overwritten bytes).
`readyup_sigcheck` also checks that every `rtti` typeinfo name still exists.

The build also runs `scripts/check_shim_binary.sh`, which fails if the shim contains direct
calls to `__cxa_pure_virtual` (an interface mirror with internal linkage) or if
`readyup_ctor` is not the last static constructor.

## Sigtest

From server console:

```text
ru sigtest
```

Prints OK/WARN/FAIL for every engine-surface function. Ready Up disables itself at startup if a
required function doesn't resolve (to avoid undefined behavior).

## Selftest

From the server console (`ru selftest`) or admin chat (`.ru selftest`, which shows the summary in
chat and the full report in the console). It prints:

- every engine-surface function: address, verified or not, and its anchors
- RTTI classes, vtable slots (verified, patched) and struct layouts
- funchook detour sites: live, or the prologue still relocates (the runtime version of `readyup_hookcheck`)
- the plugin host: API version, plugins dir, loaded plugins, load failures
- the ready HUD and the `hud_brand` / `hud_logo_url` header
- every schema field Ready Up uses, with its offset
- runtime hooks: GameFrame, ClientCommand, command buffer and log listener
- engine events: manager, listener, and whether any event has been delivered yet
- entity system, database and clientprint status
- the feature on/off table

It ends with one line: `selftest: PASS n/n` or `selftest: FAIL k/n (...)`.

For CI, start the server with `READYUP_SELFTEST_AND_QUIT=1` (or `-readyup_selftest_and_quit`).
After the first map has been simulating for `READYUP_SELFTEST_DELAY` seconds (default 5), Ready Up
runs the selftest and writes the report plus `exit_code: 0|1` to `READYUP_SELFTEST_FILE`
(default `readyup_selftest.txt` next to the shim). Then it quits with exit code 0 (PASS) or 1
(FAIL). If no map is ticking within `READYUP_SELFTEST_TIMEOUT` seconds (default 300), for example
because GameFrame could not be hooked, it writes a FAIL report and exits.

## Crash handler

On SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT, Ready Up prints a backtrace to stderr and appends it to
`readyup_crash.log` next to the shim. It then restores the default action and re-raises the
signal, so the process really dies and the supervisor can restart it. A 10 s `alarm()` backstop
kills the process if the handler itself deadlocks. Opt out with `READYUP_CRASH_HANDLER=0`.
