// ctest `playersettings_cvar_msg`: the query is laid out where the engine reads it, and an answer
// built the way the engine builds one reads back (cvar_msg.h).
#include "cvar_msg.h"

#include <cstdio>
#include <cstring>

using namespace playersettings;

static int g_failed = 0;
static void Check(bool ok, const char* what) {
  if (!ok) {
    ++g_failed;
    std::printf("FAIL: %s\n", what);
  }
}

template <typename T>
static T At(const uint8_t* p, size_t off) {
  T v{};
  std::memcpy(&v, p + off, sizeof(v));
  return v;
}

// What the engine does to read a protobuf string field: untag, follow, length at chars - 0x18.
static std::string EngineRead(const uint8_t* proto, size_t field) {
  const uintptr_t object = At<uintptr_t>(proto, field) & ~uintptr_t{3};
  const char* chars = *reinterpret_cast<const char* const*>(object);
  return std::string(chars, static_cast<size_t>(*reinterpret_cast<const uint64_t*>(chars - 0x18)));
}

int main() {
  alignas(16) static uint8_t fakeVtable[0x100] = {};
  const void* vt = fakeVtable + 0x10;

  QueryMessage m;
  Check(BuildQuery(m, vt, 77, "cl_crosshairsize"), "builds");
  Check(At<uintptr_t>(m.bytes, 0) == reinterpret_cast<uintptr_t>(vt), "CNetMessagePB vptr");
  const uint8_t* proto = ProtoOf(m.bytes);
  Check(proto == m.bytes + 0x30, "protobuf part at +0x30");
  Check(At<uintptr_t>(proto, 0) == reinterpret_cast<uintptr_t>(vt) + 0x48, "its vptr: vtable + 0x48, like Clone");
  Check(At<uint64_t>(proto, 8) == 0, "no arena, no unknown fields");
  Check(At<uint32_t>(proto, 0x10) == 3, "has-bits: name and cookie");
  Check(At<int32_t>(proto, 0x20) == 77, "cookie at +0x20");
  Check(EngineRead(proto, 0x18) == "cl_crosshairsize", "the engine reads the name back");
  Check(*(reinterpret_cast<const char*>(m.name.chars) + m.name.length) == 0, "NUL-terminated, like c_str()");
  Check(m.name.refcount == -1, "leaked: an engine copy clones it");

  Check(!BuildQuery(m, vt, 1, ""), "an empty name is refused");
  Check(!BuildQuery(m, vt, 1, std::string(200, 'x')), "a name that does not fit is refused");
  Check(!BuildQuery(m, nullptr, 1, "sensitivity"), "no vtable, no message");

  // ByteSizeLong of the query: tag + len + "sensitivity" (11) + tag + cookie.
  Check(ExpectedQuerySize(1234567, "sensitivity") == 1 + 1 + 11 + 1 + 3, "expected size");
  Check(ExpectedQuerySize(-1, "x") == 1 + 1 + 1 + 1 + 10, "negative int32: 10-byte varint");

  // An answer, laid out like the engine's CCLCMsg_RespondCvarValue.
  alignas(16) uint8_t reply[0x30] = {};
  CowString name, value;
  std::memcpy(name.chars, "sensitivity", 11);
  name.length = 11;
  std::memcpy(value.chars, "1.25", 4);
  value.length = 4;
  const char* nameObj = name.chars;
  const char* valueObj = value.chars;
  const uintptr_t n = reinterpret_cast<uintptr_t>(&nameObj) | 2;  // an allocated string's tag
  const uintptr_t v = reinterpret_cast<uintptr_t>(&valueObj);
  const uint32_t has = 1 | 2 | 4;  // no status_code: proto2 default 0
  const int32_t cookie = 42;
  std::memcpy(reply + 0x10, &has, 4);
  std::memcpy(reply + 0x18, &n, 8);
  std::memcpy(reply + 0x20, &v, 8);
  std::memcpy(reply + 0x28, &cookie, 4);
  Reply r = ReadReply(reply);
  Check(r.hasCookie && r.cookie == 42, "cookie from +0x28");
  Check(r.name == "sensitivity" && r.value == "1.25", "name and value, tag bits ignored");
  Check(r.status == kIntact, "no status: intact");

  const uint32_t has2 = 1 | 4 | 8;
  const int32_t notFound = kNotFound;
  std::memcpy(reply + 0x10, &has2, 4);
  std::memcpy(reply + 0x2C, &notFound, 4);
  r = ReadReply(reply);
  Check(r.status == kNotFound && r.value.empty(), "not found, no value");
  Check(std::strcmp(StatusName(kProtected), "protected") == 0, "status names");

  value.length = 1000;  // longer than its buffer: cut at maxLen
  std::memcpy(reply + 0x10, &has, 4);
  Check(ReadReply(reply, 8).value.size() == 8, "long values are cut at maxLen");

  std::printf("%s\n", g_failed ? "FAILED" : "OK");
  return g_failed ? 1 : 0;
}
