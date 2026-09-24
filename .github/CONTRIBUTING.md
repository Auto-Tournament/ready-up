# Contributing to Ready Up

Ready Up is part of [Auto Tournament](https://github.com/Auto-Tournament). Docs live at **[docs.autotournament.gg](https://docs.autotournament.gg)**.

## Reporting bugs

[Open an issue](https://github.com/Auto-Tournament/ready-up/issues/new/choose) with:

- your CS2 build (`version` in the server console) and Ready Up build (`ru` in the server console)
- the output of `ru selftest`
- the relevant part of the server console log

## Pull requests

1. Fork the repo and create a branch.
2. Build with `./build.sh` and run `build/readyup_sigcheck <path/to/libserver.so> gamedata/engine-surface.json`.
3. Keep engine-facing changes in `gamedata/engine-surface.json`, each with an identity anchor. See [docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md).
4. Open a pull request against `master` and describe how you tested it.

By contributing you agree that your contribution is licensed under the [PolyForm Noncommercial License 1.0.0](../LICENSE).
