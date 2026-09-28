# Releasing

Releases are built and published by CI, never from a laptop. `./release.sh` only writes
`VERSION`, commits, tags and pushes; the tag push runs `.github/workflows/build.yml`.

## Versions

`VERSION` holds a SemVer 2.0.0 version, and the tag is `v` + that version:

| Kind | Example | GitHub release | Who gets it |
|---|---|---|---|
| stable | `0.1.0` | marked **latest** | everyone: `install.sh` default, `releases/latest`, the platform's update check |
| beta | `0.1.0-beta.1` | **pre-release**, never latest | only on request: `install.sh --channel beta` or `--version v0.1.0-beta.1` |
| release candidate | `0.1.0-rc.1` | **pre-release**, never latest | same as beta |

`N` in `-beta.N` / `-rc.N` starts at 1. Pre-releases sort before their release
(`0.1.0-beta.1 < 0.1.0-beta.2 < 0.1.0-rc.1 < 0.1.0`). Any other suffix is refused by
`release.sh` and by the workflow's Version step.

The version is compiled into the core and every plugin, so a beta build reports itself as
`0.1.0-beta.1 (<commit>)` in `ru version`, the crash log, `/status` (`core_build`) and the fleet
`hello.versions.core` ([FLEET.md](FLEET.md#version-strings)). Builds that are not from a tag (PRs,
master) carry `VERSION` as it is in the tree plus the commit; their zips are named
`<VERSION>-dev.<commit>`.

## Cut a release

From a clean, up-to-date `master`:

```bash
./release.sh --dry-run 0.1.0-beta.1   # shows the VERSION change, commit, tag, push and release kind
./release.sh 0.1.0-beta.1             # asks once, then commits "Release v0.1.0-beta.1", tags and pushes
```

`./release.sh` forms:

- `./release.sh X.Y.Z`, `X.Y.Z-beta.N`, `X.Y.Z-rc.N` (a leading `v` is fine)
- `./release.sh patch|minor|major`: bumps a stable `VERSION`. While `VERSION` is a
  pre-release these are refused; name the version (`./release.sh 0.1.0` finishes `0.1.0-rc.2`).
- `./release.sh` with no version: tags the current `VERSION` as it is.
- `--dry-run` (`-n`) changes nothing and needs no GitHub login. `WATCH=0` skips following the run.

It refuses a dirty tree, a malformed version and a tag that already exists locally or on origin.

## What CI does with the tag

1. **Version**: checks the tag is `vX.Y.Z` or `vX.Y.Z-(beta|rc).N` and equals `v` + `VERSION`.
2. **Build + verify**: sniper build, unit tests, ctest, sigcheck + hookcheck against the current
   public CS2 build.
3. **Package**: `scripts/package-release.sh` (component zips, the essentials / full bundles,
   `SHA256SUMS`), bundle checks.
4. **Installer**: `tests/installer/test_install.sh` against those zips (including the beta channel),
   and the release notes are generated once as a check.
5. **Release**: `scripts/ci/release-notes.sh` writes the notes; the download table comes from the
   zips themselves (each zip's `readyup/manifests/*.json`), so a new component needs no edit there.
   A version with a suffix is published with `gh release create --prerelease` (and the notes say
   how to install it); a stable one with `--latest`. Re-running the job on an existing release
   uploads the files again and fixes the pre-release / latest flag.
6. **Discord** announcement when `DISCORD_WEBHOOK_URL` is set.

## After a beta

- Testers install or update with `--channel beta` (newest release including pre-releases) or pin
  one with `--version v0.1.0-beta.1`. A plain `install.sh` (and `--yes` updates) stays on the latest
  stable release; while none exists it stops and points to those two flags.
- Next beta: `./release.sh 0.1.0-beta.2`. Release candidate: `./release.sh 0.1.0-rc.1`.
- The final release: `./release.sh 0.1.0`. Its notes list the changes since the previous
  **stable** tag (a pre-release's notes list the changes since the previous tag of any kind).
