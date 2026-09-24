#pragma once

// Mirror of the CS2 game-event interfaces Ready Up calls into.
//
// Source of truth: alliedmodders/hl2sdk, branch `cs2`, public/igameevents.h
// (checked against commit 6315f0104d22eb9ea3c33d0505dbe14e8b193bc3).
// The real headers are not included directly because they pull in tier0/tier1,
// entity2 and generated protobuf headers; only the vtable order matters here.
//
// Verified against CS2 1.41.8.3 (buildid 25492732) libserver.so:
//   CGameEventManager vtable ("17CGameEventManager") has 17 slots:
//   [0,1] dtors, [2] LoadEventsFromFile, [3] Reset, [4] AddListener, [5] FindListener,
//   ... [16] HasEventDescriptor. CGameEventManager::Init calls slot 3 then slot 2 with
//   "resource/*.gameevents", which matches this order.
//   Engine-side IGameEventListener2 implementations
//   (CServerSideClient_GameEventLegacyProxy) have 3 slots: [0,1] dtors, [2] FireGameEvent.
//
// IMPORTANT: these types MUST have external linkage (do not move them into an
// anonymous namespace). Ready Up never defines a class deriving from
// IGameEventManager2/IGameEvent, so if they had internal linkage GCC's
// type-based devirtualization "knows" the only possible target of every
// virtual call is __cxa_pure_virtual and emits a direct call to it. That is
// exactly what aborted server-4 with "pure virtual method called" inside
// InstallGameEventsListener() on build 967edc8.

#include <cctype>
#include <cstdint>
#include <cstring>

namespace readyup::sdk {

inline constexpr uint32_t kStringTokenSeed = 0x31415926u;

// MurmurHash2 (32-bit) over ASCII-lowercased bytes; matches Valve's MurmurHash2LowerCase.
inline uint32_t MurmurHash2LowerCase(const void* key, int len, uint32_t seed) {
  const uint32_t m = 0x5bd1e995u;
  const int r = 24;
  uint32_t h = seed ^ static_cast<uint32_t>(len);
  const unsigned char* data = static_cast<const unsigned char*>(key);
  auto lc = [](unsigned char c) -> uint32_t { return static_cast<uint32_t>(std::tolower(c)); };
  while (len >= 4) {
    uint32_t k = lc(data[0]) | (lc(data[1]) << 8) | (lc(data[2]) << 16) | (lc(data[3]) << 24);
    k *= m;
    k ^= k >> r;
    k *= m;
    h *= m;
    h ^= k;
    data += 4;
    len -= 4;
  }
  switch (len) {
    case 3: h ^= lc(data[2]) << 16; [[fallthrough]];
    case 2: h ^= lc(data[1]) << 8; [[fallthrough]];
    case 1: h ^= lc(data[0]); h *= m; [[fallthrough]];
    default: break;
  }
  h ^= h >> 13;
  h *= m;
  h ^= h >> 15;
  return h;
}

struct CUtlStringToken {
  uint32_t hash = 0;
  CUtlStringToken() = default;
  explicit CUtlStringToken(const char* s) {
    if (!s || !*s) return;
    hash = MurmurHash2LowerCase(s, static_cast<int>(std::strlen(s)), kStringTokenSeed);
  }
  uint32_t GetHashCode() const { return hash; }
};

using UtlSymLargeId_t = uint32_t;
inline constexpr UtlSymLargeId_t kInvalidSymLarge = 0u;

// Layout matches hl2sdk-cs2 tier1/keyvalues3.h CKV3MemberName (GameEventKeySymbol_t).
struct CKV3MemberName {
  CUtlStringToken token;
  UtlSymLargeId_t symid = kInvalidSymLarge;
  const char* psz = "";
  CKV3MemberName() = default;
  explicit CKV3MemberName(const char* s) : token(s), symid(kInvalidSymLarge), psz(s ? s : "") {}
};

struct CPlayerSlot {
  int value;
};

class IGameEvent {
 public:
  virtual ~IGameEvent() {}
  virtual const char* GetName() const = 0;
  virtual int GetID() const = 0;

  virtual bool IsReliable() const = 0;
  virtual bool IsLocal() const = 0;
  virtual bool IsEmpty(const CKV3MemberName& keySymbol) = 0;

  virtual bool GetBool(const CKV3MemberName& keySymbol, bool defaultValue = false) = 0;
  virtual int GetInt(const CKV3MemberName& keySymbol, int defaultValue = 0) = 0;
  virtual uint64_t GetUint64(const CKV3MemberName& keySymbol, uint64_t defaultValue = 0) = 0;
  virtual float GetFloat(const CKV3MemberName& keySymbol, float defaultValue = 0.0f) = 0;
  virtual const char* GetString(const CKV3MemberName& keySymbol, const char* defaultValue = "") = 0;
  virtual void* GetPtr(const CKV3MemberName& keySymbol) = 0;

  // hl2sdk returns CEntityHandle / CEntityIndex (32-bit wrappers); only the low 32 bits are meaningful.
  virtual uint64_t GetEHandle(const CKV3MemberName& keySymbol, uint64_t defaultValue = 0) = 0;
  virtual void* GetEntity(const CKV3MemberName& keySymbol, void* fallbackInstance = nullptr) = 0;
  virtual int GetEntityIndex(const CKV3MemberName& keySymbol, int defaultValue = -1) = 0;
  virtual CPlayerSlot GetPlayerSlot(const CKV3MemberName& keySymbol) = 0;
  virtual void* GetPlayerController(const CKV3MemberName& keySymbol) = 0;
  virtual void* GetPlayerPawn(const CKV3MemberName& keySymbol) = 0;
  virtual uint64_t GetPawnEHandle(const CKV3MemberName& keySymbol) = 0;
  virtual int GetPawnEntityIndex(const CKV3MemberName& keySymbol) = 0;

  virtual void SetBool(const CKV3MemberName& keySymbol, bool value) = 0;
  virtual void SetInt(const CKV3MemberName& keySymbol, int value) = 0;
  virtual void SetUint64(const CKV3MemberName& keySymbol, uint64_t value) = 0;
  virtual void SetFloat(const CKV3MemberName& keySymbol, float value) = 0;
  virtual void SetString(const CKV3MemberName& keySymbol, const char* value) = 0;
  virtual void SetPtr(const CKV3MemberName& keySymbol, void* value) = 0;

  virtual void SetEntity(const CKV3MemberName& keySymbol, void* value) = 0;
  virtual void SetEntity(const CKV3MemberName& keySymbol, int value) = 0;

  virtual void SetPlayer(const CKV3MemberName& keySymbol, void* pawn) = 0;
  virtual void SetPlayer(const CKV3MemberName& keySymbol, CPlayerSlot value) = 0;
  virtual void SetPlayerRaw(const CKV3MemberName& controllerKeySymbol, const CKV3MemberName& pawnKeySymbol, void* pawn) = 0;

  virtual bool HasKey(const CKV3MemberName& keySymbol) = 0;
  virtual void unk001() = 0;
  virtual void* GetDataKeys() const = 0;
};

class IGameEventListener2 {
 public:
  virtual ~IGameEventListener2() {}
  virtual void FireGameEvent(IGameEvent* event) = 0;
};

class IGameEventManager2 {
 public:
  virtual ~IGameEventManager2() {}
  virtual int LoadEventsFromFile(const char* filename, bool bSearchAll) = 0;
  virtual void Reset() = 0;
  // NOTE: 1.41.8.3 returns -1 in eax for an unknown event name (al=0xff reads as true);
  // callers should check HasEventDescriptor()/FindListener() rather than trust this value.
  // Descriptors only exist after CGameEventManager::Init (Reset + LoadEventsFromFile), which
  // runs after our constructor; Reset also clears every listener list. bServerSide=true
  // registers in the server-side list that FireEvent calls (listener vtable slot 2).
  // CreateEvent returns nullptr when an event has no listeners (unless bForce), so a
  // FireEvent hook alone would never see events nobody else listens to.
  virtual bool AddListener(IGameEventListener2* listener, const char* name, bool bServerSide) = 0;
  virtual bool FindListener(IGameEventListener2* listener, const char* name) = 0;
  virtual void RemoveListener(IGameEventListener2* listener) = 0;
  virtual IGameEvent* CreateEvent(const char* name, bool bForce = false, int* pCookie = nullptr) = 0;
  virtual bool FireEvent(IGameEvent* event, bool bDontBroadcast = false) = 0;
  virtual bool FireEventClientSide(IGameEvent* event) = 0;
  virtual IGameEvent* DuplicateEvent(IGameEvent* event) = 0;
  virtual void FreeEvent(IGameEvent* event) = 0;
  virtual bool SerializeEvent(IGameEvent* event, void* ev) = 0;
  virtual IGameEvent* UnserializeEvent(const void* ev) = 0;
  virtual int LookupEventId(const char* name) = 0;
  virtual void PrintEventToString(IGameEvent* event, void* out) = 0;
  virtual bool HasEventDescriptor(const char* name) = 0;
};

}  // namespace readyup::sdk
