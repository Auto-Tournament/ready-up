#pragma once

// The pre-match values of the cvars a match config sets (`cvars{}`), so a series end can put
// them back (reset_cvars_on_series_end 1, server_settings.h).
//
// When a match config is installed (WebhookSetMatchContext), every `cvars{}` name not read yet
// is read with ru_api cvar_query (1.10): the core queues the bare cvar name and answers from the
// `<name> = <value>` console line. The match cvars go out much later (at go-live, after the cfg),
// so the value read is the one from before the match. The first value read for a name wins: a
// second match loaded over the first keeps the original values. At series end (and on
// `ru match end` / unload) ResetServerRules puts every value back with `<name> "<value>"`, then
// the snapshot is cleared. reset_cvars_on_series_end 0 keeps the match values and clears it too.
//
// Kept across `ru plugin reload match` (reload_state.cpp, key "cvar_snapshot": values and the
// names already asked) and across a server restart mid-series (state.json setting
// `ru_active_cvar_snapshot`, persisted_match_state.h: values only; the recovered match asks the
// rest again on the fresh server, which still has its pre-match values). A query still pending
// when the plugin unloads is lost: that name is not restored.
//
// Without cvar_query (a core older than 1.10) nothing is read and nothing is restored; the
// fixed warmup / team-name reset still runs.

#include <map>
#include <set>
#include <unordered_map>
#include <string>
#include <vector>

namespace readyup::cvar_snapshot {

// ---- engine-free core (unit test: tests/cvar_snapshot_test.cpp) ------------------------------

class Snapshot {
 public:
  // The names of `keys` still to read (neither read nor asked); they count as asked from now on.
  // Names are compared lowercase (cvar names are case-insensitive).
  std::vector<std::string> TakeKeysToQuery(const std::vector<std::string>& keys);
  // An answer. value NULL (unknown cvar / no answer) is ignored. The first value of a name wins.
  void Record(const std::string& name, const char* value);
  // A query that could not be sent: the name may be taken again (unless it has a value).
  void Unask(const std::string& name);
  // `<name> "<value>"` for every value, in name order. Values that cannot be quoted safely (a
  // quote, a ';' or a control character) go to *skipped as names instead.
  std::vector<std::string> RestoreCommands(std::vector<std::string>* skipped = nullptr) const;

  bool Empty() const { return values_.empty() && asked_.empty(); }
  size_t size() const { return values_.size(); }
  void Clear() {
    values_.clear();
    asked_.clear();
  }
  const std::map<std::string, std::string>& values() const { return values_; }

  // {"values":{"name":"value",...},"asked":["name",...]} (asked only with withAsked).
  std::string ToJson(bool withAsked) const;
  // Replaces the snapshot. withAsked false: values only (a restarted server asks the rest again).
  bool FromJson(const std::string& json, bool withAsked);

 private:
  std::map<std::string, std::string> values_;
  std::set<std::string> asked_;
};

// True if `value` can go out as `name "value"` without breaking the command line.
bool QuotableValue(const std::string& value);

// ---- the plugin's snapshot (cvar_snapshot_glue.cpp; game thread unless noted) -----------------

// A match config was installed: read every `cvars{}` name not read yet. Any thread (the reads
// are queued on the game thread).
void CaptureMatchCvars(const std::unordered_map<std::string, std::string>& cvars);
// Series over / match ended with reset_cvars_on_series_end 1: the commands that restore the
// pre-match values. Clears the snapshot (and its persisted copy). Any thread.
std::vector<std::string> TakeRestoreCommands();
// reset_cvars_on_series_end 0 (the match values stay) or the match was dropped: forget it. Any thread.
void Discard();

// `ru plugin reload match` (reload_state.cpp): the document and its restore (with the asked names).
std::string ReloadJson();
void RestoreReloadJson(const std::string& json);
// Server restart (match_recovery.cpp): values from state.json, before the match context is set.
void RestorePersisted();

}  // namespace readyup::cvar_snapshot
