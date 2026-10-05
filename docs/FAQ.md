# FAQ

Run Ready Up on its own. The answers below were tested on a live server; the full results
are in [docs/COMPATIBILITY.md](COMPATIBILITY.md).

**Can I run it with Metamod?** Yes. Metamod's line goes first in `gameinfo.gi` and Ready Up's
directly below it (the installer does this). If Ready Up ends up above Metamod, Metamod
doesn't load and Ready Up logs a warning with the fix.

**With CounterStrikeSharp?** It loads and nothing crashes, but we don't recommend it:
CSSharp's `Host_Say` chat hook is lost next to Ready Up, and when a CS2 update breaks
CSSharp's gamedata, CSSharp crashes the whole server. Ready Up can't stop that.

**With MatchZy, Get5 or MatchZy Enhanced?** No. Never run two match plugins:
both handle `.r`, they overwrite each other's cvars and every match gets reported twice. Use
Ready Up's match plugin. It has the same commands.

**What if I need a CounterStrikeSharp plugin?** Port it to the Ready Up plugin API. Start
with [`plugins/hello`](../plugins/hello), the plugin API in
[docs/ARCHITECTURE.md](ARCHITECTURE.md) and
[`core/include/readyup/plugin_api.h`](../core/include/readyup/plugin_api.h).

