#pragma once

// The engine side of rulesets (ruleset.h, docs/ESPORTS-MODE.md): which cfg go-live execs, the
// rule commands after it, the default player models, the halftime pause, `ru match rules`, and what
// the skins plugin asks (readyup.match.v1 inventory_locked).

#include "readyup/plugin_api.h"
#include "readyup/ruleset.h"
#include "readyup/webhook.h"

#include <string>
#include <vector>

namespace readyup {

// The effective rules of the loaded match, or readyup.cfg's preset when none is loaded.
// Cached for a quarter second (the match flow asks every tick). Any thread.
EffectiveRuleSet CurrentEffectiveRules();
// Same inputs for a given context (no cache).
EffectiveRuleSet EffectiveRulesFor(const WebhookMatchContext* ctx);

// "exec ReadyUp/live.cfg" | "exec ReadyUp/esports_live.cfg" for the loaded match.
std::string LiveCfgExecCommand();
// True under the valve ruleset: go-live execs its cfg even with ru_cfg_exec_enable 0.
bool LiveCfgRequired();
// Rule commands that go out after the go-live cfg and the match cvars (RuleCommands).
void AppendRuleCommands(std::vector<std::string>* cmds);
// Players' inventories must not be modified right now (skins plugin inert).
bool InventoryLockedNow();

// readyup_plugin_load: player_spawn (default_models), round starts (halftime pause), frames.
void EsportsInstall(const ru_api* api);
// A match was loaded: logs the ruleset, the cfg and what differs from the preset.
void EsportsOnMatchLoaded(const WebhookMatchContext& ctx);
// match_events: the regulation halftime started. With halftime_pausematch the engine pauses the
// match when the second half starts; Ready Up marks that pause ("halftime": both teams .unpause).
void EsportsOnHalftime();

// `ru match rules`.
std::vector<std::string> EsportsRulesReport();
// `ru selftest` line (INFO): ruleset + differs.
std::string EsportsSelftestLine();

}  // namespace readyup
