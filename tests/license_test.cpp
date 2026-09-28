// Offline tests for core/src/readyup/license.h: the license key check against the vectors in
// tests/fixtures/license/vectors.json (signed with a throwaway key by
// tests/license/make_vectors.mjs, which also checks them against the website's reference
// verifier), plus the embedded key table and the text helpers. ctest `license`.
#include "readyup/license.h"
#include "readyup/minijson.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef READYUP_LICENSE_VECTORS
#error "READYUP_LICENSE_VECTORS must point at tests/fixtures/license/vectors.json"
#endif

using namespace readyup;

static int g_failures = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

static const char* StatusName(license::Status s) {
  switch (s) {
    case license::Status::Ok: return "ok";
    case license::Status::Warning: return "warning";
    case license::Status::Invalid: return "invalid";
  }
  return "?";
}

static void RunVectors() {
  std::ifstream f(READYUP_LICENSE_VECTORS);
  std::stringstream ss;
  ss << f.rdbuf();
  minijson::ParseError err;
  const auto doc = minijson::Parse(ss.str(), &err);
  CHECK(doc.has_value());
  if (!doc) return;

  // The throwaway keys own strings; PublicKey holds pointers into them.
  std::vector<std::string> owned;
  const minijson::Value* keys = doc->get("keys");
  CHECK(keys && keys->type == minijson::Value::Type::Array && !keys->arr.empty());
  for (const auto& k : keys->arr) {
    owned.push_back(k.get("kid")->str);
    owned.push_back(k.get("x")->str);
  }
  std::vector<license::PublicKey> table;
  for (size_t i = 0; i + 1 < owned.size(); i += 2) table.push_back({owned[i].c_str(), owned[i + 1].c_str()});

  const minijson::Value* cases = doc->get("cases");
  CHECK(cases && cases->arr.size() >= 30);
  int n = 0;
  for (const auto& c : cases->arr) {
    ++n;
    const std::string name = c.get("name")->str;
    license::Options o;
    o.keys = &table;
    o.line_date = c.get("line_date")->str;
    o.today = c.get("today")->str;
    const license::Result r = license::Verify(c.get("token")->str, o);

    std::string codes, want;
    for (const auto& i : r.issues) codes += (codes.empty() ? "" : ",") + i.code;
    for (const auto& w : c.get("codes")->arr) want += (want.empty() ? "" : ",") + w.str;
    const std::string wantStatus = c.get("status")->str;
    if (wantStatus != StatusName(r.status) || codes != want) {
      std::fprintf(stderr, "case \"%s\": got %s [%s], want %s [%s]\n", name.c_str(), StatusName(r.status),
                   codes.c_str(), wantStatus.c_str(), want.c_str());
      ++g_failures;
    }
    // Nothing is ever "blocked": invalid keys still yield a warning line, never an error state.
    CHECK(r.valid() == (r.status != license::Status::Invalid));
    CHECK(license::ConsoleLine(r).rfind("License", 0) == 0);
    if (const minijson::Value* line = c.get("console")) {
      if (license::ConsoleLine(r) != line->str) {
        std::fprintf(stderr, "case \"%s\": console line\n  got  %s\n  want %s\n", name.c_str(),
                     license::ConsoleLine(r).c_str(), line->str.c_str());
        ++g_failures;
      }
    }
    if (const minijson::Value* player = c.get("player")) {
      if (license::PlayerLine(r) != player->str) {
        std::fprintf(stderr, "case \"%s\": player line \"%s\", want \"%s\"\n", name.c_str(),
                     license::PlayerLine(r).c_str(), player->str.c_str());
        ++g_failures;
      }
    }
    // The key itself never shows up in what is printed.
    const std::string token = c.get("token")->str;
    if (token.size() > 40) CHECK(license::ConsoleLine(r).find(token.substr(5, 30)) == std::string::npos);
  }
  std::printf("license_test: %d vectors\n", n);

  // The production table does not know the throwaway key.
  license::Options prod;
  prod.line_date = "2027-03-01";
  prod.today = "2027-03-01";
  const license::Result r = license::Verify(cases->arr.front().get("token")->str, prod);
  CHECK(r.status == license::Status::Invalid && r.issues.front().code == "unknown_kid");
  // Invalid keys only warn: the line says Ready Up runs as normal.
  CHECK(license::ConsoleLine(r).find("runs as normal") != std::string::npos);
  CHECK(license::PlayerLine(r).empty());
}

int main() {
  RunVectors();

  // Embedded keys: the website's current signing key, 32 raw bytes each.
  const auto& keys = license::EmbeddedPublicKeys();
  bool found = false;
  for (const auto& k : keys) {
    std::vector<uint8_t> raw;
    CHECK(license::Base64UrlDecode(k.x, &raw) && raw.size() == 32);
    CHECK(std::string(k.kid).size() == 16);
    if (std::string(k.kid) == "tWl_YS3_AzLgqdkm") {
      found = true;
      CHECK(std::string(k.x) == "YQMKIdrQtVz-QFV3Tw0AWa7RivnNtjK6PiJzskg8R0g");
    }
  }
  CHECK(found);

  CHECK(license::IsDate("2028-02-29") && !license::IsDate("2027-02-29") && !license::IsDate("2027-13-01"));
  CHECK(license::IsDate("9999-12-31") && !license::IsDate("2027-1-01") && !license::IsDate("2027-01-01T"));
  std::vector<uint8_t> b;
  CHECK(license::Base64UrlDecode("aGk", &b) && std::string(b.begin(), b.end()) == "hi");
  CHECK(!license::Base64UrlDecode("aG+k", &b));
  CHECK(license::Printable("a\x1b[2Jb\nc") == "a[2Jbc");
  CHECK(license::Printable("\xC3\x85\xC3\x85", 3) == "\xC3\x85");  // never cuts inside a character
  CHECK(license::IsDate(license::TodayUtc()));

  std::printf("license_test: %s\n", g_failures ? "FAIL" : "PASS");
  return g_failures ? 1 : 0;
}
