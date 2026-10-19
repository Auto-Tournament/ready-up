#include "readyup/license_checkin.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <regex>
#include <string>

#include "readyup/http_client.h"
#include "readyup/minijson.h"
#include "readyup/workers.h"

namespace readyup::license_checkin {
namespace {

constexpr const char* kDefaultUrl = "https://autotournament.gg/api/licenses/checkin";
constexpr auto kEvery = std::chrono::minutes(15);
constexpr auto kFirst = std::chrono::seconds(60);  // server.cfg sets the key after load

#ifndef MATCH_VERSION
#define MATCH_VERSION "0.0.0"
#endif

bool IsUuid(const std::string& s) {
  static const std::regex re("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$");
  return std::regex_match(s, re);
}

std::string NewUuid() {
  std::random_device rd;
  std::uniform_int_distribution<int> d(0, 15);
  const char* hex = "0123456789abcdef";
  std::string out;
  for (int i = 0; i < 32; ++i) {
    if (i == 8 || i == 12 || i == 16 || i == 20) out += '-';
    int v = d(rd);
    if (i == 12) v = 4;                 // version 4
    if (i == 16) v = 8 | (v & 3);       // variant
    out += hex[v];
  }
  return out;
}

// This server's own id: read from the data folder, or made and saved the first time.
std::string ServerUuid(const std::string& dir) {
  const std::string path = dir + "/server_id";
  std::string id;
  if (std::ifstream in(path); in >> id && IsUuid(id)) return id;
  id = NewUuid();
  std::ofstream(path) << id << "\n";
  return id;
}

std::string ReadCmdline() {
  std::ifstream in("/proc/self/cmdline", std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string Base64UrlDecode(const std::string& in) {
  std::string out;
  int val = 0, bits = -8;
  for (const char c : in) {
    int d;
    if (c >= 'A' && c <= 'Z') d = c - 'A';
    else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
    else if (c >= '0' && c <= '9') d = c - '0' + 52;
    else if (c == '-') d = 62;
    else if (c == '_') d = 63;
    else break;
    val = (val << 6) + d;
    bits += 6;
    if (bits >= 0) {
      out += static_cast<char>((val >> bits) & 0xFF);
      bits -= 8;
    }
  }
  return out;
}

void Loop(const ru_api* api, std::string dir) {
  if (!workers::SleepFor(kFirst)) return;
  const char* env = std::getenv("READYUP_LICENSE_CHECKIN_URL");  // tests point it at a local server
  const std::string url = env && *env ? env : kDefaultUrl;
  const std::string uuid = ServerUuid(dir);
  const int port = PortFromCmdline(ReadCmdline());
  do {
    char buf[4096];
    const int n = api->license_key(api->self, buf, sizeof(buf));
    if (n <= 0 || n >= static_cast<int>(sizeof(buf))) continue;
    const std::string key(buf, static_cast<size_t>(n));
    const std::string id = KeyId(key);
    if (id.empty()) continue;
    // Key, id, UUID and version are [A-Za-z0-9._+-] only (checked by the key format and IsUuid), so no escaping.
    const std::string body = std::string("{\"token\":\"") + key + "\",\"key_id\":\"" + id + "\",\"instance_id\":\"" + uuid +
                             "\",\"server_count\":1,\"platform_version\":\"" MATCH_VERSION "\",\"product\":\"readyup\",\"port\":" +
                             std::to_string(port) + "}";
    (void)HttpPostJson(url, std::nullopt, body);  // best effort: the answer changes nothing here
  } while (workers::SleepFor(kEvery));
}

}  // namespace

std::string KeyId(const std::string& token) {
  const size_t a = token.find('.');
  const size_t b = a == std::string::npos ? std::string::npos : token.find('.', a + 1);
  if (b == std::string::npos) return {};
  const auto v = minijson::Parse(Base64UrlDecode(token.substr(a + 1, b - a - 1)), nullptr);
  const minijson::Value* id = v ? v->get("id") : nullptr;
  if (!id || !minijson::IsString(id)) return {};
  static const std::regex re("^[A-Za-z0-9_-]{1,64}$");
  return std::regex_match(id->str, re) ? id->str : std::string();
}

int PortFromCmdline(const std::string& cmdline) {
  std::string prev, arg;
  for (const char c : cmdline + std::string(1, '\0')) {
    if (c != '\0') {
      arg += c;
      continue;
    }
    if (prev == "-port") {
      char* end = nullptr;
      const long p = std::strtol(arg.c_str(), &end, 10);
      if (end && *end == '\0' && p >= 1 && p <= 65535) return static_cast<int>(p);
    }
    prev = arg;
    arg.clear();
  }
  return 27015;
}

void Start(const ru_api* api, const std::string& dataDir) {
  if (!RU_API_HAS(api, license_key) || !api->license_key) return;
  (void)workers::Spawn("license check-in", [api, dataDir] { Loop(api, dataDir); });
}

}  // namespace readyup::license_checkin
