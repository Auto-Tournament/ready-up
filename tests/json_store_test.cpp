// libs/readyup/json_store: versioned JSON files (docs/FLEET.md D13). ctest `json_store`.
#include "readyup/json_store.h"

#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <string>
#include <unistd.h>

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    ++g_checks;                                                                     \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

using readyup::json_store::Json;
namespace store = readyup::json_store;

void WriteRaw(const std::string& path, const std::string& text) {
  FILE* f = std::fopen(path.c_str(), "wb");
  std::fwrite(text.data(), 1, text.size(), f);
  std::fclose(f);
}

int CountPrefix(const std::string& dir, const std::string& prefix) {
  int n = 0;
  if (DIR* d = opendir(dir.c_str())) {
    while (dirent* e = readdir(d)) n += std::string(e->d_name).rfind(prefix, 0) == 0;
    closedir(d);
  }
  return n;
}

}  // namespace

int main() {
  char tmpl[] = "/tmp/ru-json-store-XXXXXX";
  const char* base = mkdtemp(tmpl);
  if (!base) return 1;
  const std::string dir = std::string(base) + "/plugins/match";
  const std::string path = dir + "/state.json";

  Json doc;
  std::string note;
  CHECK(store::Load(path, 1, &doc, &note) == store::LoadResult::Missing);

  // Save creates the directory, puts "version" first and leaves no temp file behind.
  Json in = Json::Object();
  in["settings"]["ru_match_token"] = "abc";
  in["settings"]["ru_active_match_json"] = "{\"matchid\":\"x\"}";
  std::string err;
  CHECK(store::Save(path, in, 1, &err));
  CHECK(CountPrefix(dir, "state.json.tmp") == 0);
  CHECK(store::Load(path, 1, &doc, &note) == store::LoadResult::Ok);
  CHECK(doc.Find("version") && doc.Find("version")->AsInt() == 1);
  CHECK(doc.Members().front().first == "version");
  const Json* s = doc.Find("settings");
  CHECK(s && s->Find("ru_match_token") && s->Find("ru_match_token")->AsString() == "abc");
  CHECK(s && s->Find("ru_active_match_json")->AsString() == "{\"matchid\":\"x\"}");
  CHECK(store::MtimeNs(path) != 0);

  // Broken JSON, no version, a newer version: moved aside, reported Corrupt, start fresh.
  for (const char* bad : {"{\"settings\": {", "{\"settings\": {}}", "[1,2]", "{\"version\": 9, \"settings\": {}}"}) {
    WriteRaw(path, bad);
    note.clear();
    CHECK(store::Load(path, 1, &doc, &note) == store::LoadResult::Corrupt);
    CHECK(note.find("starting fresh") != std::string::npos);
    CHECK(store::Load(path, 1, &doc, &note) == store::LoadResult::Missing);
  }
  CHECK(CountPrefix(dir, "state.json.corrupt-") >= 1);

  {
    store::FileLock lock(path);  // lock file next to it; released at scope end
    CHECK(access((path + ".lock").c_str(), F_OK) == 0);
  }

  std::string cmd = std::string("rm -rf '") + base + "'";
  (void)std::system(cmd.c_str());
  if (g_failures) {
    std::fprintf(stderr, "json_store_test: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("json_store_test: all %d checks passed\n", g_checks);
  return 0;
}
