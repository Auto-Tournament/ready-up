// Offline tests for core/src/readyup/license_status.h: the `readyup_license_key` /
// `readyup_show_license` console settings, the csgo/cfg/readyup_license.cfg fallback that CS2
// Server Manager writes, the license answer install.sh records (readyup_license_accepted in
// csgo/cfg/ReadyUp/license.cfg), and that nothing printed contains the key. ctest `license_status`.
#include "readyup/license.h"
#include "readyup/license_status.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

// ---- stubs for the core pieces license_status.cpp uses -----------------------------------
static std::vector<std::string> g_log;
static std::string g_csgoDir;

namespace readyup {
void Print(const char* fmt, ...) {
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  g_log.push_back(buf);
}
void PrintLine(const char* msg) { g_log.push_back(msg); }
std::string GetCsgoDirFromModuleDir() { return g_csgoDir; }
}  // namespace readyup

using namespace readyup;

static int g_failures = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

static bool Has(const std::string& s, const std::string& n) { return s.find(n) != std::string::npos; }
// The line about what players see (the last line is the license-terms answer).
static std::string PlayersLine() {
  const auto l = license::StatusLines();
  return l.size() >= 2 ? l[l.size() - 2] : std::string();
}
static bool LogHas(const std::string& n) {
  for (const auto& l : g_log) {
    if (Has(l, n)) return true;
  }
  return false;
}

// A key-shaped token no build trusts (unknown kid). The check says why; nothing is blocked.
static const std::string kFileKey =
    "ATL1.eyJ2IjoxLCJraWQiOiJmaWxlS2V5X19fX19fX19fIiwiaWQiOiJMLWZpbGUifQ."
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
static const std::string kConsoleKey =
    "ATL1.eyJ2IjoxLCJraWQiOiJjb25zb2xlX19fX19fX19fIiwiaWQiOiJMLWNvbiJ9."
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

int main() {
  char tmpl[] = "/tmp/readyup_license_status_XXXXXX";
  const char* dir = mkdtemp(tmpl);
  CHECK(dir != nullptr);
  if (!dir) return 1;
  g_csgoDir = dir;
  mkdir((g_csgoDir + "/cfg").c_str(), 0700);

  // Nothing set anywhere: no line at load (server.cfg may still set it), the free-use line in status.
  license::LogAtLoad();
  CHECK(g_log.empty());
  CHECK(license::StatusLines().front() == license::kNoKeyLine);
  CHECK(license::PlayerLineIfShown().empty());
  // No license answer recorded: a notice in `ru license`, never anything else.
  CHECK(license::AcceptedUse().empty());
  CHECK(license::StatusLines().back() == license::kNotAcceptedLine);
  CHECK(Has(license::kNotAcceptedLine, "run the installer") && Has(license::kNotAcceptedLine, "readyup_license_accepted"));

  // Other commands are not ours.
  CHECK(!license::HandleConsoleLine("sv_cheats 1"));
  CHECK(!license::HandleConsoleLine("readyup_license_keys \"x\""));
  CHECK(!license::HandleConsoleLine("exec readyup_license.cfg"));

  // csm's file (comments first) is read before server.cfg has run.
  {
    std::ofstream f(g_csgoDir + "/cfg/readyup_license.cfg");
    f << "// Written by csm: the Auto Tournament license key for Ready Up.\n"
      << "// Change it with `csm license set` / `csm license clear`; edits here are overwritten.\n"
      << "readyup_license_key \"" << kFileKey << "\"\n";
  }
  license::LogAtLoad();
  CHECK(g_log.size() == 1 && Has(g_log[0], "License warning: the license key is not valid"));
  CHECK(Has(g_log[0], "runs as normal"));
  auto lines = license::StatusLines();
  CHECK(lines.size() >= 3 && Has(lines[1], "readyup_license.cfg") && Has(lines[1], "version line"));
  CHECK(Has(lines[2], "unknown_kid"));
  license::LogAtLoad();  // the same key again: no second line
  CHECK(g_log.size() == 1);

  // server.cfg execs the file: the same key, nothing new. A new key: one line.
  CHECK(license::HandleConsoleLine("readyup_license_key \"" + kFileKey + "\""));
  CHECK(g_log.size() == 1);
  CHECK(license::HandleConsoleLine("  readyup_license_key " + kConsoleKey + "  "));
  CHECK(g_log.size() == 2);
  CHECK(Has(license::StatusLines()[1], "key from readyup_license_key"));
  CHECK(license::HandleConsoleLine("readyup_license_key \"" + kConsoleKey + "\""));  // map change: same key
  CHECK(g_log.size() == 2);

  // Bare name: the state, without the key.
  CHECK(license::HandleConsoleLine("readyup_license_key"));
  CHECK(g_log.size() == 3 && Has(g_log[2], "readyup_license_key is set"));

  // Players: nothing by default, and nothing for a key that does not verify.
  CHECK(license::HandleConsoleLine("readyup_show_license 1"));
  CHECK(license::PlayerLineIfShown().empty());
  CHECK(Has(PlayersLine(), "no licensee"));
  CHECK(license::HandleConsoleLine("readyup_show_license \"0\""));
  CHECK(Has(PlayersLine(), "players see nothing"));

  // Cleared on the console: no key, even with the file still there.
  CHECK(license::HandleConsoleLine("readyup_license_key \"\""));
  CHECK(LogHas("License key cleared"));
  CHECK(license::StatusLines().front() == license::kNoKeyLine);

  // `ru reload`: the line again when a key is set.
  const size_t before = g_log.size();
  CHECK(license::HandleConsoleLine("readyup_license_key \"" + kConsoleKey + "\""));
  license::LogOnReload();
  CHECK(g_log.size() == before + 2);

  // The installer's answer: cfg/ReadyUp/license.cfg, read without the file being exec'd.
  mkdir((g_csgoDir + "/cfg/ReadyUp").c_str(), 0700);
  {
    std::ofstream f(g_csgoDir + "/cfg/ReadyUp/license.cfg");
    f << "// Written by the Ready Up installer\n"
      << "readyup_license_accepted \"noncommercial\"\n"
      << "readyup_license_accepted_at \"2026-09-29T10:00:00Z\"\n";
  }
  CHECK(license::AcceptedUse() == "noncommercial");
  CHECK(Has(license::StatusLines().back(), "accepted for noncommercial use on 2026-09-29T10:00:00Z"));
  CHECK(Has(license::StatusLines().back(), "ReadyUp/license.cfg"));
  // The same settings from a cfg / the console win; any case; an unknown answer counts as none.
  CHECK(!license::HandleConsoleLine("readyup_license_acceptedx 1"));
  CHECK(license::HandleConsoleLine("readyup_license_accepted \"Commercial\""));
  CHECK(license::HandleConsoleLine("readyup_license_accepted_at \"2026-10-01\""));
  CHECK(license::AcceptedUse() == "commercial");
  CHECK(Has(license::StatusLines().back(), "commercial use on 2026-10-01 (readyup_license_accepted)"));
  const size_t n = g_log.size();
  CHECK(license::HandleConsoleLine("readyup_license_accepted"));  // bare: the state
  CHECK(g_log.size() == n + 1 && Has(g_log.back(), "accepted for commercial use"));
  CHECK(license::HandleConsoleLine("readyup_license_accepted maybe"));
  CHECK(license::AcceptedUse().empty());
  CHECK(license::StatusLines().back() == license::kNotAcceptedLine);

  // The key never reaches the log.
  for (const auto& l : g_log) {
    CHECK(!Has(l, kFileKey.substr(5, 40)) && !Has(l, kConsoleKey.substr(5, 40)));
  }

  std::remove((g_csgoDir + "/cfg/readyup_license.cfg").c_str());
  std::remove((g_csgoDir + "/cfg/ReadyUp/license.cfg").c_str());
  rmdir((g_csgoDir + "/cfg/ReadyUp").c_str());
  rmdir((g_csgoDir + "/cfg").c_str());
  rmdir(g_csgoDir.c_str());
  std::printf("license_status_test: %s\n", g_failures ? "FAIL" : "PASS");
  return g_failures ? 1 : 0;
}
