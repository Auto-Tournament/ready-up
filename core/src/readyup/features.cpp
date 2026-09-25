#include "readyup/features.h"

#include "readyup/client_command_hook.h"
#include "readyup/command_buffer_hook.h"
#include "readyup/db_config.h"
#include "readyup/engine_surface.h"
#include "readyup/game_events.h"
#include "readyup/game_frame_hook.h"
#include "readyup/logging.h"
#include "readyup/postgres.h"
#include "readyup/schema.h"
#include "readyup/server_game_clients_hook.h"
#include "readyup/entity.h"

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>

namespace readyup {
namespace {

using Clock = std::chrono::steady_clock;
using Group = std::vector<const char*>;  // any one member satisfies the group

struct FeatureDef {
  Feature id;
  const char* name;
  std::vector<Group> needs;  // all groups must be satisfied
};

// The single source of truth for "which engine surface does each feature need".
// fn:X      engine-surface function X resolved (unique signature + anchors)
// vtable:X  engine-surface vtable slot X verified (RTTI vtable + slot anchors)
// hook:X    that verified slot is patched on the live interface object
// layout:X  engine-surface struct layout X (pinned by verified code)
// cmdbuf    CCommandBuffer::AddText hooked (exported symbol, GOT patch): server commands/cfg exec
// loglistener  tier0 LoggingSystem listener registered (chat + lifecycle log lines)
// schema    SchemaSystem located (RTTI) and its class-info layout verified
// entsys    entity system global located and its layout round-trip verified (after a map loads)
// eventmgr  CGameEventManager located and RTTI-verified
// db        Postgres compiled in and configured (readyup_db.json)
const std::vector<FeatureDef>& Defs() {
  static const std::vector<FeatureDef> defs = {
      {Feature::ChatCommands, "chat_commands", {{"loglistener", "hook:ClientCommand"}, {"fn:UTIL_ClientPrintAll", "cmdbuf"}}},
      {Feature::MatchFlow, "match_flow", {{"hook:GameFrame"}, {"cmdbuf"}}},
      {Feature::Pauses, "pauses", {{"cmdbuf"}, {"loglistener", "hook:ClientCommand"}}},
      {Feature::WelcomeHtml, "welcome_html", {{"hook:GameFrame"}, {"fn:LegacyGameEventListener"}, {"eventmgr"}}},
      {Feature::RoundTermSuppression, "round_term_suppression", {{"fn:CCSGameRules_TerminateRound"}}},
      {Feature::Events, "events", {{"fn:CGameEventManager_Init"}, {"eventmgr"}}},
      {Feature::ClientCommandHook, "client_command_hook", {{"hook:ClientCommand"}, {"layout:CCommand"}}},
      {Feature::PlayerChatPrint, "player_chat_print", {{"fn:ClientPrint"}}},
      // Same per-client center-HTML path as the welcome card.
      {Feature::ReadyHud, "ready_hud", {{"hook:GameFrame"}, {"fn:LegacyGameEventListener"}, {"eventmgr"}}},
      {Feature::HudBrand, "hud_brand", {{"feature:welcome_html", "feature:ready_hud"}}},
      // Kills/HP come from log lines; knife.cfg + restarts go through the command buffer.
      {Feature::Knife, "knife", {{"feature:match_flow"}, {"loglistener"}, {"cmdbuf"}}},
      {Feature::Plugins, "plugins", {{"hook:GameFrame"}}},
  };
  return defs;
}

const FeatureDef* DefOf(Feature f) {
  for (const auto& d : Defs())
    if (d.id == f) return &d;
  return nullptr;
}

const FeatureDef* DefByName(const std::string& n) {
  for (const auto& d : Defs())
    if (n == d.name) return &d;
  return nullptr;
}

DepStatus Ok(std::string d = {}) { return {DepStatus::State::Ok, std::move(d)}; }
DepStatus Pending(std::string d) { return {DepStatus::State::Pending, std::move(d)}; }
DepStatus Fail(std::string d) { return {DepStatus::State::Fail, std::move(d)}; }

DepStatus FromTri(int tri, const std::string& detail) {
  if (tri == 1) return Ok(detail);
  if (tri == 2) return Fail(detail);
  return Pending(detail);
}

struct Eval {
  DepStatus::State state = DepStatus::State::Pending;
  std::string missing;
};

Eval Evaluate(const FeatureDef& def, int depth);

DepStatus DependencyStatusImpl(const std::string& dep, int depth) {
  auto after = [&](const char* prefix) -> std::string { return dep.substr(std::char_traits<char>::length(prefix)); };

  if (dep.rfind("fn:", 0) == 0) {
    const std::string name = after("fn:");
    if (!RealServerImage()) return Pending("real libserver.so not loaded");
    const es::Resolution r = EngineFunctionResolution(name.c_str());
    return r.ok ? Ok(r.detail) : Fail(r.detail);
  }
  if (dep.rfind("vtable:", 0) == 0) {
    const VtableVerdict v = EngineVtableVerdict(after("vtable:").c_str());
    if (!v.checked) return Pending(v.detail.empty() ? "not verified yet" : v.detail);
    return v.ok ? Ok(v.detail) : Fail(v.detail);
  }
  if (dep.rfind("layout:", 0) == 0) {
    const std::string name = after("layout:");
    for (const auto& l : EngineLayoutReport()) {
      if (l.name == name) return l.ok ? Ok("verified by " + l.verified_by) : Fail(l.verified_by + " unverified");
    }
    return Fail("not listed in engine-surface.json");
  }
  if (dep == "hook:GameFrame") {
    if (GameFrameHookInstalled()) return Ok("patched");
    std::string why;
    if (GameFrameHookFailed(&why)) return Fail(why);
    return Pending("not installed yet");
  }
  if (dep == "hook:ClientCommand") {
    if (ServerGameClientsHookInstalled()) return Ok("patched");
    std::string why;
    if (ServerGameClientsHookFailed(&why)) return Fail(why);
    return Pending("not installed yet");
  }
  if (dep == "cmdbuf") {
    return CommandBufferHookInstalled() ? Ok("CCommandBuffer::AddText hooked")
                                        : Fail("CCommandBuffer::AddText import not found/patched");
  }
  if (dep == "loglistener") {
    if (InProcessLogListenerActive()) return Ok("registered");
    return Fail("LoggingSystem_RegisterLoggingListener unavailable");
  }
  if (dep == "schema") {
    std::string d;
    const int tri = SchemaStatus(&d);
    return FromTri(tri, d);
  }
  if (dep == "entsys") {
    std::string d;
    const int tri = entity::EntitySystemStatus(&d);
    return FromTri(tri, d);
  }
  if (dep == "eventmgr") {
    std::string d;
    const int tri = GameEventManagerStatus(&d);
    return FromTri(tri, d);
  }
  if (dep == "events_live") {
    // Engine game events are delivered (they then drive the round lifecycle instead of log lines).
    if (GameEventsListenerInstalled()) return Ok("engine events delivered");
    return Pending("round lifecycle from log lines");
  }
  if (dep == "db") {
    if (!pg::Available()) return Fail("built without Postgres");
    if (!DbCfg()) return Fail("readyup_db.json missing/invalid");
    return Ok("configured");
  }
  if (dep.rfind("feature:", 0) == 0) {
    const FeatureDef* d = DefByName(after("feature:"));
    if (!d || depth > 4) return Fail("unknown feature");
    const Eval e = Evaluate(*d, depth + 1);
    if (e.state == DepStatus::State::Ok) return Ok("on");
    if (e.state == DepStatus::State::Fail) return Fail("off (" + e.missing + ")");
    return Pending("pending (" + e.missing + ")");
  }
  return Fail("unknown dependency");
}

Eval Evaluate(const FeatureDef& def, int depth) {
  Eval out;
  out.state = DepStatus::State::Ok;
  for (const auto& group : def.needs) {
    // Group: Ok if any member is Ok; Fail if all members Fail; otherwise Pending.
    bool anyOk = false, allFail = true;
    std::string groupWhy;
    for (const char* dep : group) {
      const DepStatus s = DependencyStatusImpl(dep, depth);
      if (s.state == DepStatus::State::Ok) {
        anyOk = true;
        break;
      }
      if (s.state != DepStatus::State::Fail) allFail = false;
      if (!groupWhy.empty()) groupWhy += " | ";
      groupWhy += std::string(dep) + " (" + s.detail + ")";
    }
    if (anyOk) continue;
    if (allFail) {
      out.state = DepStatus::State::Fail;
      out.missing = groupWhy;
      return out;  // one missing group is enough to say why
    }
    if (out.state == DepStatus::State::Ok) {
      out.state = DepStatus::State::Pending;
      out.missing = groupWhy;
    }
  }
  return out;
}

// Per-feature settled state: 0 unsettled (pending), 1 on, 2 off. Settled states are final: the
// dependencies they depend on do not come back once they failed, and do not go away once
// verified (later runtime problems are handled inside the feature).
constexpr size_t kN = static_cast<size_t>(Feature::kCount);
std::array<std::atomic<int>, kN>& Settled() {
  static std::array<std::atomic<int>, kN> a{};
  return a;
}
std::array<std::atomic<long long>, kN>& LastEvalMs() {
  static std::array<std::atomic<long long>, kN> a{};
  return a;
}

long long NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

}  // namespace

const char* FeatureName(Feature f) {
  const FeatureDef* d = DefOf(f);
  return d ? d->name : "?";
}

DepStatus DependencyStatus(const std::string& dep) { return DependencyStatusImpl(dep, 0); }

bool FeatureEnabled(Feature f) {
  const size_t i = static_cast<size_t>(f);
  if (i >= kN) return false;
  const int st = Settled()[i].load(std::memory_order_acquire);
  if (st == 1) return true;
  if (st == 2) return false;

  // Pending: re-evaluate at most every 250 ms (dependency probes are cheap but not free).
  const long long now = NowMs();
  const long long last = LastEvalMs()[i].load(std::memory_order_relaxed);
  if (last != 0 && now - last < 250) return false;
  LastEvalMs()[i].store(now, std::memory_order_relaxed);

  const FeatureDef* def = DefOf(f);
  if (!def) return false;
  const Eval e = Evaluate(*def, 0);
  if (e.state == DepStatus::State::Pending) return false;
  const int want = (e.state == DepStatus::State::Ok) ? 1 : 2;
  int expected = 0;
  if (Settled()[i].compare_exchange_strong(expected, want) && want == 2) {
    // The one line an operator needs; `ru selftest` has the full picture.
    Print("feature %s DISABLED: needs %s. Everything else keeps working.\n", def->name, e.missing.c_str());
  }
  return Settled()[i].load(std::memory_order_acquire) == 1;
}

int FeatureStateByName(const std::string& name) {
  if (const FeatureDef* d = DefByName(name)) {
    if (FeatureEnabled(d->id)) return 1;
    return Settled()[static_cast<size_t>(d->id)].load(std::memory_order_acquire) == 2 ? -1 : 0;
  }
  const DepStatus s = DependencyStatus(name);
  return s.state == DepStatus::State::Ok ? 1 : s.state == DepStatus::State::Fail ? -1 : 0;
}

std::vector<FeatureReport> FeatureReports() {
  std::vector<FeatureReport> out;
  for (const auto& def : Defs()) {
    (void)FeatureEnabled(def.id);  // settle if possible (logs the disable line once)
    FeatureReport r;
    r.name = def.name;
    for (const auto& g : def.needs) {
      std::vector<std::string> names;
      for (const char* d : g) names.emplace_back(d);
      r.needs.push_back(std::move(names));
    }
    const int st = Settled()[static_cast<size_t>(def.id)].load(std::memory_order_acquire);
    const Eval e = Evaluate(def, 0);
    r.state = st == 1 ? "on" : st == 2 ? "off" : "pending";
    r.missing = e.missing;
    out.push_back(std::move(r));
  }
  return out;
}

}  // namespace readyup
