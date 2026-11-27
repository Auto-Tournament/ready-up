// The two CS2 net messages behind a client cvar query, built and read as raw memory (no protobuf
// in Ready Up). Engine-free, so ctest `playersettings_cvar_msg` checks it against fake memory.
//
// svc_GetCvarValue (58): the server asks a client for one cvar. CNetMessagePB<58,
// CSVCMsg_GetCvarValue> is 0x58 bytes: the CNetMessage header (vptr + receive bookkeeping) up to
// 0x30, then the protobuf message. clc_RespondCvarValue (25) is the client's answer, same shape.
// Every offset below is pinned by an anchor in gamedata/engine-surface.playersettings.json on the
// engine's own ByteSizeLong / AsMessageLite code: if CS2 moves one, the plugin stays off.
//
// Protobuf strings on CS2 Linux are libstdc++'s copy-on-write std::string: one pointer to the
// characters, with the length 0x18 bytes before them. The query points the field at a string of
// ours marked "leaked" (refcount -1), so an engine copy always deep-copies it and nothing of the
// engine's ever frees it; the buffer stays ours.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace playersettings {

constexpr size_t kNetMsgSize = 0x58;  // sizeof(CNetMessagePB<...>) for both messages (Clone: new 0x58)
constexpr size_t kProtoAt = 0x30;     // the protobuf message inside it (AsMessageLite: lea rax, [rdi+0x30])
constexpr size_t kProtoVtableAt = 0x48;  // its vptr: the CNetMessagePB vtable + 0x48 (Clone)

// Protobuf message fields (from kProtoAt): has-bits, cached size, then the fields.
constexpr size_t kHasBits = 0x10;
constexpr size_t kGetName = 0x18, kGetCookie = 0x20;  // CSVCMsg_GetCvarValue: bit 1 cvar_name, bit 2 cookie
constexpr size_t kRespName = 0x18, kRespValue = 0x20, kRespCookie = 0x28, kRespStatus = 0x2C;  // bits 1, 2, 4, 8

// eQueryCvarValueStatus.
enum CvarStatus : int { kIntact = 0, kNotFound = 1, kNotACvar = 2, kProtected = 3 };
const char* StatusName(int status);

// A copy-on-write string body: what the engine's std::string points at (the characters), with
// its length, capacity and refcount in front.
struct CowString {
  uint64_t length = 0;
  uint64_t capacity = 0;
  int32_t refcount = -1;  // "leaked": never shared, so a copy clones it
  int32_t pad = 0;
  char chars[128] = {};
};
static_assert(offsetof(CowString, chars) == 0x18, "the length sits 0x18 bytes before the characters");

// One query message: the engine object and the strings it points at, all ours.
struct QueryMessage {
  alignas(16) uint8_t bytes[kNetMsgSize] = {};
  CowString name;
  const char* nameObject = nullptr;  // the std::string object: a pointer to name.chars
};

// Fills `m` as CNetMessagePB<58, CSVCMsg_GetCvarValue> with this cookie and cvar name. netVtable:
// the class's primary vtable (address point). False if the name does not fit.
bool BuildQuery(QueryMessage& m, const void* netVtable, int32_t cookie, const std::string& cvar);

// The protobuf message of `msg` (a CNetMessagePB).
inline const uint8_t* ProtoOf(const void* msg) { return static_cast<const uint8_t*>(msg) + kProtoAt; }

struct Reply {
  bool hasCookie = false;
  int32_t cookie = 0;
  int status = -1;
  std::string name, value;
};
// Reads a CCLCMsg_RespondCvarValue protobuf message (ProtoOf the received CNetMessagePB). Strings
// longer than maxLen are cut there.
Reply ReadReply(const uint8_t* proto, size_t maxLen = 256);

// What ByteSizeLong should return for the query: tag + length + name, tag + cookie varint.
size_t ExpectedQuerySize(int32_t cookie, const std::string& cvar);

}  // namespace playersettings
