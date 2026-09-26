// needs.json -> load / disable decision (core/src/readyup/plugin_needs.h). ctest `plugin_needs`.
#include "readyup/plugin_needs.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <unistd.h>

namespace rp = readyup::plugins;

static int g_failed = 0;
static void Check(bool ok, const std::string& what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
  if (!ok) ++g_failed;
}

// A fake CS2 build: which surface entries resolved, which schema fields exist, which events.
struct Build {
  std::map<std::string, int> surface;  // missing key: -1 (cannot tell)
  std::map<std::string, int> schema;   // "Class.m_field" -> offset
  std::map<std::string, int> events;
  bool schemaReady = true;
  rp::NeedsProbe Probe() const {
    rp::NeedsProbe p;
    p.surface = [this](const std::string& n) {
      auto it = surface.find(n);
      return it == surface.end() ? -1 : it->second;
    };
    p.schema = [this](const std::string& c, const std::string& f) {
      if (!schemaReady) return -2;
      auto it = schema.find(c + "." + f);
      return it == schema.end() ? -1 : it->second;
    };
    p.event = [this](const std::string& e) {
      auto it = events.find(e);
      return it == events.end() ? -1 : it->second;
    };
    p.cs2Build = "25537370";
    return p;
  }
};

int main() {
  const char* kJson = R"({"schema_version":1,"plugin":"match",
    "api":["chat_all","schema_offset"],
    "surface":["UTIL_ClientPrintAll","ISource2Server::GameFrame"],
    "schema":["CCSPlayerController.m_iTeamNum|CBaseEntity.m_iTeamNum","CBasePlayerController.m_steamID"],
    "schema_optional":["CCSPlayerController.m_iKills"],
    "events":["round_start","round_mvp"]})";
  std::string err;
  const auto n = rp::ParseNeeds(kJson, &err);
  Check(n && err.empty(), "parse a manifest");
  Check(n && n->plugin == "match" && n->surface.size() == 2 && n->schema.size() == 2 && n->schemaOptional.size() == 1 &&
            n->events.size() == 2,
        "parsed every list");
  Check(!rp::ParseNeeds("{\"surface\": \"Host_Say\"}", &err) && err.find("not an array") != std::string::npos,
        "a non-array list is an error");
  Check(!rp::ParseNeeds("[1,", &err), "broken JSON is an error");

  Build good;
  good.surface = {{"UTIL_ClientPrintAll", 1}, {"ISource2Server::GameFrame", 1}};
  good.schema = {{"CBaseEntity.m_iTeamNum", 0x3cb}, {"CBasePlayerController.m_steamID", 0x6d0},
                 {"CCSPlayerController.m_iKills", 0x10}};
  good.events = {{"round_start", 1}, {"round_mvp", 1}};
  {
    const rp::NeedsVerdict v = rp::EvaluateNeeds(*n, good.Probe());
    Check(v.load && v.reason.empty() && v.warnings.empty(), "everything present: load");
    Check(v.checks.size() == 7, "one check per entry");
    Check(v.checks[2].state == 1 && v.checks[2].detail == "0x3cb (CBaseEntity.m_iTeamNum)",
          "second schema alternative satisfies the need");
  }
  {
    Build b = good;
    b.surface["UTIL_ClientPrintAll"] = 0;
    const rp::NeedsVerdict v = rp::EvaluateNeeds(*n, b.Probe());
    Check(!v.load && v.reason == "missing UTIL_ClientPrintAll after CS2 build 25537370", "unresolved surface: disabled");
    Check(rp::NeedsDisabledLine("match", v.reason) ==
              "plugin[match] disabled: missing UTIL_ClientPrintAll after CS2 build 25537370",
          "log line format");
  }
  {
    Build b = good;
    b.schema.erase("CBasePlayerController.m_steamID");
    b.schema.erase("CBaseEntity.m_iTeamNum");
    b.surface["ISource2Server::GameFrame"] = 0;
    const rp::NeedsVerdict v = rp::EvaluateNeeds(*n, b.Probe());
    Check(!v.load && v.reason ==
                         "missing ISource2Server::GameFrame, CCSPlayerController.m_iTeamNum|CBaseEntity.m_iTeamNum, "
                         "CBasePlayerController.m_steamID after CS2 build 25537370",
          "required schema field gone: disabled, every missing need named");
  }
  {
    Build b = good;
    b.schema.erase("CCSPlayerController.m_iKills");
    b.events["round_mvp"] = 0;
    const rp::NeedsVerdict v = rp::EvaluateNeeds(*n, b.Probe());
    Check(v.load && v.warnings.size() == 2, "optional schema field and unknown event: load with warnings");
    Check(v.warnings[1] == "game event round_mvp unknown to this CS2 build", "event warning text");
  }
  {
    Build b = good;
    b.schemaReady = false;
    b.surface.clear();
    b.events.clear();
    const rp::NeedsVerdict v = rp::EvaluateNeeds(*n, b.Probe());
    bool allPending = true;
    for (const auto& c : v.checks) allPending = allPending && c.state == -1;
    Check(v.load && allPending, "nothing checkable yet: load, every need pending");
  }
  {
    std::string e2;
    Check(!rp::LoadNeedsFile("/nonexistent/x.needs.json", &e2) && e2.empty(), "no manifest: nothing to enforce");
    char tmpl[] = "/tmp/ru-needs-XXXXXX";
    const int fd = mkstemp(tmpl);
    if (fd >= 0) {
      const std::string body = "{\"plugin\":\"x\",\"surface\":[\"Host_Say\"]}";
      Check(write(fd, body.data(), body.size()) == static_cast<ssize_t>(body.size()), "write temp manifest");
      close(fd);
      const auto f = rp::LoadNeedsFile(tmpl, &e2);
      Check(f && f->surface.size() == 1 && f->surface[0] == "Host_Say", "manifest read from disk");
      unlink(tmpl);
    }
  }
  std::printf("%s (%d failed)\n", g_failed ? "FAILED" : "ALL OK", g_failed);
  return g_failed ? 1 : 0;
}
