#pragma once

// Load-order checks for running next to Metamod:Source (docs/COMPATIBILITY.md).
//
// Supported chain:  engine -> Metamod's libserver.so -> Ready Up's libserver.so -> Valve's.
// gameinfo.gi must list `Game csgo/addons/metamod` directly ABOVE `Game csgo/readyup`
// (scripts/patch_gameinfo.py enforces it). With the lines the other way round the engine loads
// Ready Up first, Ready Up loads Valve's library directly, and Metamod (with CounterStrikeSharp)
// silently never loads. That is not a crash, so Ready Up keeps running, but it says so loudly.

#include <string>

namespace readyup {

struct GameSearchPaths {
  int readyup = -1;   // index among the `Game <path>` lines of SearchPaths, -1 = absent
  int metamod = -1;
  int csgo = -1;
  int readyup_count = 0;
};

// Pure: parses the SearchPaths block of a gameinfo.gi text.
GameSearchPaths ParseGameSearchPaths(const std::string& gameinfoText);

// Pure: "" when the order is fine (or cannot be judged), otherwise one line saying what is
// wrong and what it causes.
std::string DescribeLoadOrderProblem(const GameSearchPaths& p);

// Pure: decides whether this shim copy may run, given the value of the process-wide marker
// left by a copy that already claimed the process ("<pid> <module path>", empty if none).
// Returns true when this copy is the first (or the same one) in process `pid`.
bool ShimInstanceMayRun(const std::string& marker, long pid, const std::string& ourPath, std::string* otherPath);

// Runtime (readyup_ctor):
// Claims the process for this shim copy. False when another Ready Up shim already runs in this
// process (two copies in the search paths, or a copy chained behind another): this copy must
// stay inert and only forward to Valve's library, or every hook would be installed twice.
bool ClaimShimInstance(std::string* otherPath);

// Reads <csgo>/gameinfo.gi and logs one warning per problem; notes when Metamod is loaded.
void CheckLoadOrder();

// True when Metamod:Source's server library is loaded in this process.
bool MetamodLoaded();

}  // namespace readyup
