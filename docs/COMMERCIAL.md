# Commercial use and license keys

The free license covers noncommercial use only. If you or your organization earn money from
Ready Up (a business, a profit-making event or tournament, a paid server operator, or selling
Ready Up or a service built on it), you need a paid commercial license before you install it.
See [pricing](https://autotournament.gg/pricing) or email
[sivert@autotournament.gg](mailto:sivert@autotournament.gg). Then install with
`--accept-license=commercial --license-key ATL1...` (or answer "commercial" in the installer). The
server prints a one-line license notice at every start, and a notice when no license answer is
recorded (`readyup_license_accepted`); neither ever blocks anything.

## License key

 A paid license comes with a key (`ATL1...`). Put it in `server.cfg` (or let
[CS2 Server Manager](https://github.com/Auto-Tournament/cs2-server-manager) do it: `csm license set`
writes `cfg/readyup_license.cfg` and execs it from `server.cfg`):

```
readyup_license_key "ATL1...."
readyup_show_license 0   // 1: players see "Licensed to <licensee>" in .help and .ru version
```

Ready Up checks the key offline (Ed25519, no network) and prints one console line at load and
when the key changes, e.g. `License: Example LAN · Servers M (15 servers) · yearly, updates until
2027-10-01 · valid (license L-…)`. `ru license` (console) or `.ru license` (admins) shows it again.
The key itself is never logged. **The key never blocks, disables or slows anything**: an expired
update period (this build's version line came out after `updates_until`), an event window that
has ended, or an invalid key is a console warning only, and players never see anything like
"unlicensed". Without a key, the server logs one line that it is free for non-commercial use.

Looking for an MIT plugin instead? [MatchZy Enhanced](https://github.com/Auto-Tournament/matchzy-enhanced) is MIT licensed and free for any use, including paid work. Ready Up is a different plugin, not a fork of it.

Third-party code under `third_party/` keeps its own license.
