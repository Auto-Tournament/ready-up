// readyup-playersettings: reads players' CS2 settings (crosshair, sensitivity, viewmodel, HUD)
// from their game. Step 1 of the player-settings feature (2026-11-26): the cvar query itself and a
// test command; the platform side comes later.
//
// How: the server sends svc_GetCvarValue with the client's own CServerSideClient::SendNetMessage
// (as the SDK's CNetMessage::Send; PostEventAbstract is for game events and never sent it), and
// the answer, clc_RespondCvarValue, arrives in CServerSideClient::ProcessRespondCvarValue (a
// `return true` stub in CS2, which never asks itself), which this plugin hooks. The client comes
// from NetworkServerService_001 -> GetNetworkServer -> the server's client vector. Every engine
// piece is an entry in gamedata/engine-surface.playersettings.json, checked at load: when a CS2
// update moves one, this plugin turns itself off and match, fleet and the rest keep working.
//
//   ru cvar <slot|name> <cvar> [cvar ...]   ask a player (console / admins); answers print here
//   ru cvar selftest                        build a query and check it against the engine's own
//                                           ByteSizeLong (no player needed)
//   ru cvar status                          what is verified, the client vector, answers seen
#include "cvar_msg.h"
#include "readyup/plugin_api.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <sys/uio.h>
#include <unistd.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using namespace playersettings;

const ru_api* g_api = nullptr;

constexpr const char* kHookKey = "CServerSideClient::ProcessRespondCvarValue";
constexpr const char* kSendKey = "CServerSideClient::SendNetMessage";
constexpr const char* kGetServerKey = "INetworkServerService::GetNetworkServer";
constexpr const char* kSerializerKey = "CNetMessagePB<GetCvarValue>::GetSerializerPB";
constexpr const char* kReplyProtoKey = "CNetMessagePB<RespondCvarValue>::AsMessageLite";
constexpr const char* kQuerySizeKey = "CSVCMsg_GetCvarValue::ByteSizeLong";
constexpr const char* kReplySizeKey = "CCLCMsg_RespondCvarValue::ByteSizeLong";
constexpr int kBufReliable = 1;  // NetChannelBufType_t BUF_RELIABLE
constexpr double kTimeoutSeconds = 5.0;

using ProcessRespondFn = bool (*)(void* client, const void* msg);
using SendNetMessageFn = bool (*)(void* client, const void* msg, int bufType);
using GetNetworkServerFn = void* (*)(void* service);
using GetSerializerFn = void* (*)(const void* msg);
using ByteSizeFn = size_t (*)(const void* proto);

struct Engine {
  bool ready = false;
  std::string why = "not checked yet";
  const void* netVtable = nullptr;    // CNetMessagePB<58, CSVCMsg_GetCvarValue>
  const void* clientVtable = nullptr;  // CServerSideClient
  const void* serverVtable = nullptr;  // CNetworkGameServer
  GetSerializerFn serializer = nullptr;
  ProcessRespondFn original = nullptr;
};
Engine g_engine;

// Where the server keeps its client vector (CNetworkGameServerBase::m_Clients), found by scanning
// (the SDK's offset would drift with CS2 updates): -1 = not found yet.
long g_clientsAt = -1;
int g_clientsLayout = 0;  // 1: {int size; T* elements}, 2: {T* elements; int alloc; int grow; int size}
std::atomic<int> g_hookCalls{0}, g_hookNoCookie{0};

// Fake objects whose first word is a verified vtable: what hook_vtable / surface_vtable_function
// want as "the object" when we have none of our own.
struct FakeObject {
  const void* vptr = nullptr;
};
FakeObject g_clientObject, g_queryProtoObject, g_replyProtoObject, g_netObject;

using Clock = std::chrono::steady_clock;
struct Pending {
  int slot = -1;
  std::string cvar;
  Clock::time_point sent;
};
std::unordered_map<int32_t, Pending> g_pending;  // game thread
int32_t g_nextCookie = 0x5253;                  // "RS"; any positive start

std::mutex g_replyMu;
std::deque<Reply> g_replies;  // filled by the hook (whatever thread the engine calls it on)

// The query messages stay alive a while after posting, in case the engine reads them late.
std::deque<std::unique_ptr<QueryMessage>> g_sent;

void Log(const char* fmt, const std::string& a = "", const std::string& b = "", const std::string& c = "") {
  ru_logf(g_api, RU_LOG_INFO, fmt, a.c_str(), b.c_str(), c.c_str());
}

// Reads n bytes of this process's memory without faulting on a bad address.
bool SafeRead(uintptr_t addr, void* out, size_t n) {
  iovec local{out, n}, remote{reinterpret_cast<void*>(addr), n};
  return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(n);
}

bool OnProcessRespondCvarValue(void* client, const void* msg) {
  g_hookCalls.fetch_add(1, std::memory_order_relaxed);
  if (msg) {
    Reply r = ReadReply(ProtoOf(msg));
    if (!r.hasCookie) g_hookNoCookie.fetch_add(1, std::memory_order_relaxed);
    if (r.hasCookie) {
      std::lock_guard<std::mutex> lk(g_replyMu);
      if (g_replies.size() < 256) g_replies.push_back(std::move(r));
    }
  }
  return g_engine.original ? g_engine.original(client, msg) : true;
}

// Checks every entry and installs the hook. Game thread, at load.
void Prepare() {
  Engine e;
  auto fail = [&](const std::string& why) {
    e.why = why;
    g_engine = e;
    ru_logf(g_api, RU_LOG_WARN, "playersettings: off: %s (match, fleet and the rest are not affected)", why.c_str());
  };
  e.netVtable = g_api->surface_rtti_vtable(g_api->self, "NetMsg_GetCvarValue");
  e.clientVtable = g_api->surface_rtti_vtable(g_api->self, "CServerSideClient");
  const void* queryProto = g_api->surface_rtti_vtable(g_api->self, "CSVCMsg_GetCvarValue");
  const void* replyProto = g_api->surface_rtti_vtable(g_api->self, "CCLCMsg_RespondCvarValue");
  const void* replyNet = g_api->surface_rtti_vtable(g_api->self, "NetMsg_RespondCvarValue");
  if (!e.netVtable || !e.clientVtable || !queryProto || !replyProto || !replyNet) return fail("an RTTI class was not found");
  // The protobuf part's vptr is the CNetMessagePB vtable + 0x48 (what Clone writes): there must
  // be a secondary vtable there, offset_to_top -0x30, same typeinfo as the primary.
  const auto* vt = static_cast<const uint8_t*>(e.netVtable);
  int64_t top = 0;
  const void *ti = nullptr, *ti2 = nullptr;
  std::memcpy(&top, vt + kProtoVtableAt - 16, sizeof(top));
  std::memcpy(&ti, vt - 8, sizeof(ti));
  std::memcpy(&ti2, vt + kProtoVtableAt - 8, sizeof(ti2));
  if (top != -static_cast<int64_t>(kProtoAt) || ti != ti2) return fail("CNetMessagePB<GetCvarValue> has another shape");

  g_netObject.vptr = e.netVtable;
  g_queryProtoObject.vptr = queryProto;
  g_replyProtoObject.vptr = replyProto;
  g_clientObject.vptr = e.clientVtable;
  FakeObject replyNetObject{replyNet};
  // Layout anchors: these slots are only ever checked, never called through the fake objects.
  if (!g_api->surface_vtable_function(g_api->self, kQuerySizeKey, &g_queryProtoObject))
    return fail(std::string(kQuerySizeKey) + " did not verify");
  if (!g_api->surface_vtable_function(g_api->self, kReplySizeKey, &g_replyProtoObject))
    return fail(std::string(kReplySizeKey) + " did not verify");
  if (!g_api->surface_vtable_function(g_api->self, kReplyProtoKey, &replyNetObject))
    return fail(std::string(kReplyProtoKey) + " did not verify");
  e.serializer = reinterpret_cast<GetSerializerFn>(g_api->surface_vtable_function(g_api->self, kSerializerKey, &g_netObject));
  if (!e.serializer) return fail(std::string(kSerializerKey) + " did not verify");
  e.serverVtable = g_api->surface_rtti_vtable(g_api->self, "CNetworkGameServer");
  if (!e.serverVtable) return fail("CNetworkGameServer was not found");
  if (!g_api->surface_vtable_function(g_api->self, kSendKey, &g_clientObject))
    return fail(std::string(kSendKey) + " did not verify");

  void* original = nullptr;
  if (!g_api->hook_vtable(g_api->self, kHookKey, &g_clientObject, reinterpret_cast<void*>(&OnProcessRespondCvarValue),
                          &original) ||
      !original) {
    return fail(std::string(kHookKey) + " could not be hooked");
  }
  e.original = reinterpret_cast<ProcessRespondFn>(original);
  e.ready = true;
  e.why = "ready";
  g_engine = e;
  ru_logf(g_api, RU_LOG_INFO, "playersettings: cvar queries ready");
}

// The running network server (CNetworkGameServer), RTTI-checked, or nullptr. Game thread.
void* NetworkServer(std::string* why) {
  void* service = g_api->engine_interface(g_api->self, "NetworkServerService_001", "CNetworkServerService");
  if (!service) {
    *why = "no NetworkServerService";
    return nullptr;
  }
  auto get = reinterpret_cast<GetNetworkServerFn>(g_api->surface_vtable_function(g_api->self, kGetServerKey, service));
  if (!get) {
    *why = std::string(kGetServerKey) + " did not verify";
    return nullptr;
  }
  void* server = get(service);
  uintptr_t vptr = 0;
  if (!server || !SafeRead(reinterpret_cast<uintptr_t>(server), &vptr, sizeof(vptr)) ||
      vptr != reinterpret_cast<uintptr_t>(g_engine.serverVtable)) {
    *why = "no running CNetworkGameServer";
    return nullptr;
  }
  return server;
}

// A client vector candidate at `at` in the server object: {count, elements}, where every non-null
// element is a CServerSideClient (verified vtable) and at least one is there.
bool ReadClients(const uint8_t* block, size_t at, int layout, std::vector<uintptr_t>* out) {
  int32_t size = 0;
  uintptr_t elems = 0;
  if (layout == 1) {
    std::memcpy(&size, block + at, 4);
    std::memcpy(&elems, block + at + 8, 8);
  } else {
    std::memcpy(&elems, block + at, 8);
    std::memcpy(&size, block + at + 16, 4);
  }
  if (size < 1 || size > 64 || !elems || (elems & 7)) return false;
  std::vector<uintptr_t> v(static_cast<size_t>(size));
  if (!SafeRead(elems, v.data(), v.size() * 8)) return false;
  int clients = 0;
  for (uintptr_t e : v) {
    if (!e) continue;
    uintptr_t vptr = 0;
    if (!SafeRead(e, &vptr, 8) || vptr != reinterpret_cast<uintptr_t>(g_engine.clientVtable)) return false;
    ++clients;
  }
  if (clients == 0) return false;
  *out = std::move(v);
  return true;
}

// The CServerSideClient in `slot`, or nullptr. Game thread.
void* ClientInSlot(int slot, std::string* why) {
  void* server = NetworkServer(why);
  if (!server) return nullptr;
  constexpr size_t kScan = 0x20000;  // the vector sits in the first part of a ~3 MB object
  std::vector<uint8_t> block(kScan + 32);
  if (!SafeRead(reinterpret_cast<uintptr_t>(server), block.data(), block.size())) {
    *why = "the network server is not readable";
    return nullptr;
  }
  std::vector<uintptr_t> clients;
  if (g_clientsAt < 0 || !ReadClients(block.data(), static_cast<size_t>(g_clientsAt), g_clientsLayout, &clients)) {
    g_clientsAt = -1;
    for (size_t at = 0; at + 24 <= kScan && g_clientsAt < 0; at += 8) {
      for (int layout : {1, 2}) {
        if (ReadClients(block.data(), at, layout, &clients)) {
          g_clientsAt = static_cast<long>(at);
          g_clientsLayout = layout;
          ru_logf(g_api, RU_LOG_INFO, "playersettings: client vector at server+0x%lx (layout %d, %zu slots)",
                  static_cast<unsigned long>(at), layout, clients.size());
          break;
        }
      }
    }
    if (g_clientsAt < 0) {
      *why = "the server's client list was not found";
      return nullptr;
    }
  }
  if (slot < 0 || static_cast<size_t>(slot) >= clients.size() || !clients[static_cast<size_t>(slot)]) {
    *why = "no client in that slot";
    return nullptr;
  }
  return reinterpret_cast<void*>(clients[static_cast<size_t>(slot)]);
}

// Sends one query. Game thread.
bool Query(int slot, const std::string& cvar, std::string* why) {
  if (!g_engine.ready) {
    *why = g_engine.why;
    return false;
  }
  if (slot < 0 || slot > 63) {
    *why = "no such slot";
    return false;
  }
  auto m = std::make_unique<QueryMessage>();
  const int32_t cookie = g_nextCookie++;
  if (g_nextCookie <= 0) g_nextCookie = 1;
  if (!BuildQuery(*m, g_engine.netVtable, cookie, cvar)) {
    *why = "bad cvar name";
    return false;
  }
  void* client = ClientInSlot(slot, why);
  if (!client) return false;
  auto send = reinterpret_cast<SendNetMessageFn>(g_api->surface_vtable_function(g_api->self, kSendKey, client));
  if (!send) {
    *why = std::string(kSendKey) + " did not verify on this client";
    return false;
  }
  if (!send(client, m->bytes, kBufReliable)) {
    *why = "SendNetMessage refused it";
    return false;
  }
  g_pending[cookie] = Pending{slot, cvar, Clock::now()};
  g_sent.push_back(std::move(m));
  while (g_sent.size() > 64) g_sent.pop_front();
  return true;
}

// Builds a query and has the engine's own ByteSizeLong measure it: the fields must land where
// the engine reads them.
void SelfTest() {
  if (!g_engine.ready) return Log("playersettings selftest: FAIL (%s)", g_engine.why);
  auto size = reinterpret_cast<ByteSizeFn>(g_api->surface_vtable_function(g_api->self, kQuerySizeKey, &g_queryProtoObject));
  if (!size) return Log("playersettings selftest: FAIL (%s did not verify)", kQuerySizeKey);
  int bad = 0;
  for (const char* cvar : {"sensitivity", "cl_crosshairsize", "viewmodel_offset_x"}) {
    QueryMessage m;
    const int32_t cookie = 1234567;
    BuildQuery(m, g_engine.netVtable, cookie, cvar);
    const size_t got = size(ProtoOf(m.bytes));
    const size_t want = ExpectedQuerySize(cookie, cvar);
    if (got != want) {
      ++bad;
      ru_logf(g_api, RU_LOG_WARN, "playersettings selftest: %s: engine size %zu, expected %zu", cvar, got, want);
    }
  }
  QueryMessage probe;
  BuildQuery(probe, g_engine.netVtable, 1, "sensitivity");
  ru_logf(g_api, RU_LOG_INFO, "playersettings selftest: %s (query layout %s; svc_GetCvarValue serializer %p)",
          bad ? "FAIL" : "PASS", bad ? "wrong" : "matches the engine", g_engine.serializer(probe.bytes));
}

void Status() {
  std::string why = "ok";
  void* server = g_engine.ready ? NetworkServer(&why) : nullptr;
  ru_logf(g_api, RU_LOG_INFO,
          "playersettings: %s; network server %p (%s); client vector %s0x%lx; answers seen %d (%d without a cookie); "
          "%zu waiting",
          g_engine.why.c_str(), server, why.c_str(), g_clientsAt < 0 ? "not found " : "at +",
          static_cast<unsigned long>(g_clientsAt < 0 ? 0 : g_clientsAt), g_hookCalls.load(), g_hookNoCookie.load(),
          g_pending.size());
}

void Tick(void*, const ru_tick_info*) {
  std::deque<Reply> got;
  {
    std::lock_guard<std::mutex> lk(g_replyMu);
    got.swap(g_replies);
  }
  for (const Reply& r : got) {
    auto it = g_pending.find(r.cookie);
    if (it == g_pending.end()) continue;  // not ours, or timed out
    ru_logf(g_api, RU_LOG_INFO, "playersettings: slot %d %s = \"%s\" (%s)", it->second.slot,
            r.name.empty() ? it->second.cvar.c_str() : r.name.c_str(), r.value.c_str(), StatusName(r.status));
    g_pending.erase(it);
  }
  const auto now = Clock::now();
  for (auto it = g_pending.begin(); it != g_pending.end();) {
    if (std::chrono::duration<double>(now - it->second.sent).count() > kTimeoutSeconds) {
      ru_logf(g_api, RU_LOG_WARN, "playersettings: slot %d %s: no answer in %.0fs", it->second.slot,
              it->second.cvar.c_str(), kTimeoutSeconds);
      it = g_pending.erase(it);
    } else {
      ++it;
    }
  }
}

// A slot from "3", or the first connected human whose name contains `who` (case-insensitive).
int FindSlot(const std::string& who) {
  if (!who.empty() && std::all_of(who.begin(), who.end(), [](char c) { return c >= '0' && c <= '9'; })) {
    return std::atoi(who.c_str());
  }
  auto lower = [](std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  const std::string want = lower(who);
  for (int slot = 0; slot <= 63; ++slot) {
    ru_player p{};
    p.struct_size = sizeof(p);
    if (g_api->get_player(g_api->self, slot, &p) && p.connected && !p.is_bot && lower(p.name).find(want) != std::string::npos)
      return slot;
  }
  return -1;
}

void OnRuCvar(void*, const ru_command_ctx* c) {
  if (!c->is_console && g_api->is_admin(g_api->self, c->steamid64) != 1) return;
  // argv: "ru" "cvar" ...
  if (c->argc >= 3 && std::strcmp(c->argv[2], "selftest") == 0) return SelfTest();
  if (c->argc >= 3 && std::strcmp(c->argv[2], "status") == 0) return Status();
  if (c->argc < 4) {
    ru_logf(g_api, RU_LOG_INFO, "usage: ru cvar <slot|name> <cvar> [cvar ...] | ru cvar selftest | ru cvar status (%s)",
            g_engine.why.c_str());
    return;
  }
  const int slot = FindSlot(c->argv[2]);
  if (slot < 0) return Log("playersettings: no player \"%s\"", c->argv[2]);
  for (int i = 3; i < c->argc; ++i) {
    std::string why;
    if (!Query(slot, c->argv[i], &why)) {
      ru_logf(g_api, RU_LOG_WARN, "playersettings: %s: not sent (%s)", c->argv[i], why.c_str());
    }
  }
}

}  // namespace

extern "C" {

READYUP_PLUGIN_EXPORT const ru_plugin_info* readyup_plugin_info(void) {
  static const ru_plugin_info info = {
      sizeof(ru_plugin_info),
      (1u << 16) | 22u,  // needs API 1.22 (engine_interface, surface_vtable_function)
      "playersettings",
      PLAYERSETTINGS_VERSION,
      "Ready Up",
      "Reads players' CS2 settings (crosshair, sensitivity, viewmodel, HUD) from their game",
  };
  return &info;
}

READYUP_PLUGIN_EXPORT int readyup_plugin_load(const ru_api* api, uint32_t core_api_version) {
  if (RU_API_VERSION_MAJOR(core_api_version) != 1 || !RU_API_HAS(api, surface_vtable_function)) return 1;
  g_api = api;
  g_api->register_ru_subcommand(g_api->self, "cvar", &OnRuCvar, nullptr);
  g_api->on_tick(g_api->self, &Tick, nullptr);
  Prepare();
  return 0;
}

READYUP_PLUGIN_EXPORT void readyup_plugin_unload(void) {
  // The core removes the vtable hook after this returns; no reply can reach us afterwards.
  g_pending.clear();
  g_sent.clear();
  g_engine = Engine{};
  g_clientsAt = -1;
  g_api = nullptr;
}

}  // extern "C"
