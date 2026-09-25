// The step-3 protocol schemas (plugins/fleet/protocol/v1: match.defs.json, messages/match.*,
// cmd*, state.*, event.*, server.availability; proposed, docs/fleet-step3-platform-notes.md)
// against the example frames in plugins/fleet/protocol/examples/v1: every example envelope and
// payload must validate, and a few broken copies must not. Examples captured from the live test
// (scripts/livetest/fleet_livetest.py --save-examples) are real frames Ready Up sent. ctest
// `fleet_protocol`.
#include "fleet_json.h"
#include "fleet_store.h"
#include "schema_check.h"

#include <dirent.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;
const std::string kBase = "https://auto-tournament.dev/fleet/v1/";

#define CHECK(cond)                                                                 \
  do {                                                                              \
    ++g_checks;                                                                     \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

using fleet::json::Value;

bool ValidateFrame(const schema::Set& set, const Value& env, const std::string& what, bool quiet = false) {
  std::vector<std::string> errs;
  bool ok = set.Validate(env, kBase + "envelope.json", &errs);
  const Value* type = env.Get("type");
  const Value* payload = env.Get("payload");
  const std::string id = kBase + "messages/" + (type ? type->AsStr() : std::string("?")) + ".json";
  if (!set.Has(id)) {
    errs.push_back("no schema for type " + (type ? type->AsStr() : std::string("?")));
    ok = false;
  } else if (payload) {
    ok = set.Validate(*payload, id, &errs) && ok;
  }
  if (!ok && !quiet) {
    std::fprintf(stderr, "%s:\n", what.c_str());
    for (const auto& e : errs) std::fprintf(stderr, "  %s\n", e.c_str());
  }
  return ok;
}

Value Load(const std::string& path) {
  std::string text;
  Value v;
  if (!fleet::ReadFile(path, &text) || !fleet::json::Parse(text, &v)) std::fprintf(stderr, "cannot read %s\n", path.c_str());
  return v;
}

}  // namespace

int main() {
  schema::Set set;
  std::string err;
  if (!set.LoadDir(FLEET_PROTOCOL_DIR, &err)) {
    std::fprintf(stderr, "schemas: %s\n", err.c_str());
    return 1;
  }
  for (const char* t : {"match.assign", "match.update", "match.unassign", "cmd", "cmd.result", "state.snapshot",
                        "state.patch", "state.request", "server.availability", "event.round_end", "event.map_result",
                        "event.backup", "event.phase", "event.pause", "event.demo", "event.series_end"}) {
    CHECK(set.Has(kBase + "messages/" + t + ".json"));
  }

  // Every example validates.
  std::vector<std::string> files;
  if (DIR* d = opendir(FLEET_EXAMPLES_DIR)) {
    while (dirent* e = readdir(d)) {
      const std::string n = e->d_name;
      if (n.size() > 5 && n.compare(n.size() - 5, 5, ".json") == 0) files.push_back(n);
    }
    closedir(d);
  }
  std::sort(files.begin(), files.end());
  CHECK(files.size() >= 10);
  for (const auto& f : files) {
    const Value env = Load(std::string(FLEET_EXAMPLES_DIR) + "/" + f);
    CHECK(ValidateFrame(set, env, f));
  }

  // Broken copies do not.
  auto broken = [&](const char* file, void (*mutate)(Value*)) {
    Value env = Load(std::string(FLEET_EXAMPLES_DIR) + "/" + file);
    mutate(env.Get("payload"));
    return !ValidateFrame(set, env, file, /*quiet=*/true);
  };
  CHECK(broken("match.assign.json", [](Value* p) { p->Get("config")->Set("password", Value::Str("has space")); }));
  CHECK(broken("match.assign.json", [](Value* p) { p->Set("epoch", Value::Int(0)); }));
  CHECK(broken("match.assign.json", [](Value* p) {
    p->Get("config")->Get("maps")->a[0].Set("sides", Value::Str("ct"));
  }));
  CHECK(broken("match.update.json", [](Value* p) {
    p->Get("ops")->a[0].Set("op", Value::Str("teleport"));
  }));
  CHECK(broken("cmd.pause.json", [](Value* p) { p->Set("name", Value::Str("rm_rf")); }));
  CHECK(broken("cmd.result.json", [](Value* p) { p->Set("status", Value::Str("maybe")); }));
  CHECK(broken("state.patch.json", [](Value* p) { p->Set("rev", Value::Int(0)); }));
  CHECK(broken("event.pause.json", [](Value* p) { p->Get("data")->Set("type", Value::Str("coffee")); }));

  if (g_failures) {
    std::fprintf(stderr, "fleet_protocol_test: %d of %d checks FAILED (%zu examples)\n", g_failures, g_checks, files.size());
    return 1;
  }
  std::printf("fleet_protocol_test: all %d checks passed (%zu schemas, %zu examples)\n", g_checks, set.size(),
              files.size());
  return 0;
}
