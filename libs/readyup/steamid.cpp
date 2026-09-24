#include "readyup/steamid.h"

#include <cstdlib>
#include <string>

namespace readyup {
namespace {

static uint64_t Steam2ToSteam64(std::string_view steam2) {
  // STEAM_X:Y:Z
  // steamid64 = Z*2 + Y + 76561197960265728
  const std::string s(steam2);
  if (s.rfind("STEAM_", 0) != 0) return 0;
  const size_t p1 = s.find(':');
  const size_t p2 = s.find(':', p1 == std::string::npos ? 0 : p1 + 1);
  if (p1 == std::string::npos || p2 == std::string::npos) return 0;
  const std::string yStr = s.substr(p1 + 1, p2 - (p1 + 1));
  const std::string zStr = s.substr(p2 + 1);
  const unsigned long long y = std::strtoull(yStr.c_str(), nullptr, 10);
  const unsigned long long z = std::strtoull(zStr.c_str(), nullptr, 10);
  return static_cast<uint64_t>(76561197960265728ULL + z * 2ULL + y);
}

static uint64_t Steam3ToSteam64(std::string_view steam3) {
  // Common in CS2 logs: [U:1:ACCOUNTID]
  // SteamID64 (individual) = 76561197960265728 + ACCOUNTID
  std::string s(steam3);
  if (!s.empty() && s.front() == '[' && s.back() == ']') {
    s = s.substr(1, s.size() - 2);
  }

  // Expect: U:1:<accountid>
  if (s.rfind("U:", 0) != 0) return 0;
  const size_t p1 = s.find(':');          // after U
  const size_t p2 = s.find(':', p1 + 1);  // after universe
  if (p2 == std::string::npos) return 0;
  const std::string universeStr = s.substr(p1 + 1, p2 - (p1 + 1));
  if (universeStr != "1") return 0;  // Only handle the usual case for now.
  const std::string accountStr = s.substr(p2 + 1);
  if (accountStr.empty()) return 0;
  const unsigned long long account = std::strtoull(accountStr.c_str(), nullptr, 10);
  return static_cast<uint64_t>(76561197960265728ULL + account);
}

}  // namespace

uint64_t ParseSteamId64Loose(std::string_view sv) {
  const std::string s(sv);
  if (s.empty()) return 0;

  // Raw steamid64.
  if (s.find_first_not_of("0123456789") == std::string::npos) {
    const unsigned long long v = std::strtoull(s.c_str(), nullptr, 10);
    return static_cast<uint64_t>(v);
  }

  // Steam2.
  if (s.rfind("STEAM_", 0) == 0) return Steam2ToSteam64(s);

  // Steam3.
  if (s.rfind("[U:", 0) == 0 || s.rfind("U:", 0) == 0) return Steam3ToSteam64(s);

  return 0;
}

}  // namespace readyup

