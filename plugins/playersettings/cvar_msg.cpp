#include "cvar_msg.h"

#include <cstring>

namespace playersettings {

const char* StatusName(int status) {
  switch (status) {
    case kIntact: return "ok";
    case kNotFound: return "not found";
    case kNotACvar: return "not a cvar";
    case kProtected: return "protected";
  }
  return "unknown";
}

bool BuildQuery(QueryMessage& m, const void* netVtable, int32_t cookie, const std::string& cvar) {
  if (!netVtable || cvar.empty() || cvar.size() >= sizeof(m.name.chars)) return false;
  std::memset(m.bytes, 0, sizeof(m.bytes));
  m.name = CowString{};
  std::memcpy(m.name.chars, cvar.data(), cvar.size());
  m.name.length = cvar.size();
  m.name.capacity = cvar.size();
  m.nameObject = m.name.chars;

  const uintptr_t vt = reinterpret_cast<uintptr_t>(netVtable);
  const uintptr_t protoVt = vt + kProtoVtableAt;
  std::memcpy(m.bytes, &vt, sizeof(vt));
  uint8_t* proto = m.bytes + kProtoAt;
  std::memcpy(proto, &protoVt, sizeof(protoVt));
  // +0x08 InternalMetadata stays 0: no arena, no unknown fields.
  const uint32_t has = 1u | 2u;  // cvar_name, cookie
  std::memcpy(proto + kHasBits, &has, sizeof(has));
  // The ArenaStringPtr: a pointer to the std::string object, tag bits 0.
  const uintptr_t str = reinterpret_cast<uintptr_t>(&m.nameObject);
  std::memcpy(proto + kGetName, &str, sizeof(str));
  std::memcpy(proto + kGetCookie, &cookie, sizeof(cookie));
  return true;
}

static std::string ReadCow(const uint8_t* field, size_t maxLen) {
  uintptr_t tagged = 0;
  std::memcpy(&tagged, field, sizeof(tagged));
  const uintptr_t object = tagged & ~uintptr_t{3};
  if (!object) return {};
  const char* chars = *reinterpret_cast<const char* const*>(object);
  if (!chars) return {};
  uint64_t len = 0;
  std::memcpy(&len, chars - 0x18, sizeof(len));
  if (len > maxLen) len = maxLen;
  return std::string(chars, static_cast<size_t>(len));
}

Reply ReadReply(const uint8_t* proto, size_t maxLen) {
  Reply r;
  if (!proto) return r;
  uint32_t has = 0;
  std::memcpy(&has, proto + kHasBits, sizeof(has));
  if (has & 1u) r.name = ReadCow(proto + kRespName, maxLen);
  if (has & 2u) r.value = ReadCow(proto + kRespValue, maxLen);
  if (has & 4u) {
    r.hasCookie = true;
    std::memcpy(&r.cookie, proto + kRespCookie, sizeof(r.cookie));
  }
  if (has & 8u) std::memcpy(&r.status, proto + kRespStatus, sizeof(r.status));
  else r.status = kIntact;  // proto2 default 0
  return r;
}

static size_t VarintSize(uint64_t v) {
  size_t n = 1;
  while (v >= 0x80) {
    v >>= 7;
    ++n;
  }
  return n;
}

size_t ExpectedQuerySize(int32_t cookie, const std::string& cvar) {
  // int32 fields encode negatives as 10-byte varints (sign extension to 64 bits).
  const uint64_t c = static_cast<uint64_t>(static_cast<int64_t>(cookie));
  return 1 + VarintSize(cvar.size()) + cvar.size() + 1 + VarintSize(c);
}

}  // namespace playersettings
