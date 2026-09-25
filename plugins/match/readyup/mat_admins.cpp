#include "readyup/mat_admins.h"

#include "readyup/http_client.h"
#include "readyup/logging.h"
#include "readyup/minijson.h"
#include "readyup/steamid.h"
#include "readyup/workers.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

namespace readyup::mat_admins {
namespace {

struct State {
  std::mutex mu;
  std::condition_variable cv;
  std::atomic<bool> started{false};
  bool stop = false;

  std::string url;
  // 0 = fetch only when explicitly triggered (startup/config change/match load).
  int refreshSeconds = 0;
  std::optional<std::string> bearerToken;

  std::unordered_set<uint64_t> admins;
  bool lastFetchOk = false;
  uint64_t lastFetchUnixSec = 0;

  bool dirty = false;  // config changed; fetch ASAP
};

State& St() {
  static State st;
  return st;
}

static std::unordered_set<uint64_t> ParseAdminsFromJson(const std::string& json, bool* okOut) {
  using namespace readyup::minijson;
  if (okOut) *okOut = false;

  ParseError perr;
  auto rootOpt = Parse(json, &perr);
  if (!rootOpt) return {};
  const Value& root = *rootOpt;

  std::unordered_set<uint64_t> out;

  // Shape A: { "admins": ["7656...", ...] }
  if (const Value* admins = root.get("admins"); admins && admins->type == Value::Type::Array) {
    for (const auto& v : admins->arr) {
      if (v.type == Value::Type::String) {
        const uint64_t sid = ParseSteamId64Loose(v.str);
        if (sid) out.insert(sid);
      } else if (v.type == Value::Type::Number) {
        const uint64_t sid = static_cast<uint64_t>(v.num);
        if (sid) out.insert(sid);
      }
    }
    if (okOut) *okOut = true;
    return out;
  }

  // Shape B: { "players": [ { "id": "7656...", "isAdmin": true }, ... ] }
  if (const Value* players = root.get("players"); players && players->type == Value::Type::Array) {
    for (const auto& pv : players->arr) {
      if (pv.type != Value::Type::Object) continue;
      const Value* isAdmin = pv.get("isAdmin");
      if (!isAdmin || isAdmin->type != Value::Type::Bool || !isAdmin->b) continue;

      uint64_t sid = 0;
      if (auto idStr = AsString(pv.get("id"))) sid = ParseSteamId64Loose(*idStr);
      if (!sid) {
        if (auto sidStr = AsString(pv.get("steamid64"))) sid = ParseSteamId64Loose(*sidStr);
      }
      if (!sid) {
        if (auto sidStr = AsString(pv.get("steamid"))) sid = ParseSteamId64Loose(*sidStr);
      }
      if (!sid) {
        if (auto idNum = AsInt(pv.get("id"))) sid = static_cast<uint64_t>(*idNum);
      }

      if (sid) out.insert(sid);
    }
    if (okOut) *okOut = true;
    return out;
  }

  return {};
}

static void ThreadMain() {
  auto& st = St();
  std::unique_lock<std::mutex> lk(st.mu);

  while (!st.stop) {
    // Wait until configured.
    if (st.url.empty()) {
      st.cv.wait(lk, [&] { return st.stop || !st.url.empty() || st.dirty; });
      if (st.stop) break;
    }

    // Snapshot config.
    const std::string url = st.url;
    const int refresh = st.refreshSeconds;
    const std::optional<std::string> token = st.bearerToken;
    st.dirty = false;

    lk.unlock();

    bool parseOk = false;
    HttpResponse r = HttpGet(url, token);
    std::unordered_set<uint64_t> parsed;
    if (r.status == 200 && !r.body.empty()) {
      parsed = ParseAdminsFromJson(r.body, &parseOk);
    }

    const bool ok = (r.status == 200) && parseOk;

    lk.lock();
    if (st.url == url) {
      st.lastFetchOk = ok;
      st.lastFetchUnixSec = static_cast<uint64_t>(std::time(nullptr));
      if (ok) {
        st.admins = std::move(parsed);
      }
    }

    if (st.stop) break;

    // Wait for refresh interval or config change.
    // refreshSeconds=0 disables periodic polling; we only refetch when marked dirty.
    if (refresh <= 0) {
      st.cv.wait(lk, [&] { return st.stop || st.dirty; });
      continue;
    }
    const auto wakeAt = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(1, refresh));
    st.cv.wait_until(lk, wakeAt, [&] { return st.stop || st.dirty; });
  }
}

static void StartThreadIfNeededLocked(State& st) {
  if (st.started.load()) return;
  st.started.store(true);
  st.stop = false;
  // A tracked worker: unload wakes it (stop) and joins it before the image goes away.
  workers::AddWaker([] {
    auto& s = St();
    {
      std::lock_guard<std::mutex> lk(s.mu);
      s.stop = true;
    }
    s.cv.notify_all();
  });
  if (!workers::Spawn("mat-admins", ThreadMain)) st.started.store(false);
}

static int ClampRefreshSeconds(int seconds) {
  // 0 disables periodic refresh.
  if (seconds <= 0) return 0;
  if (seconds < 10) return 10;
  if (seconds > 3600) return 3600;
  return seconds;
}

}  // namespace

void ConfigureAdminsUrl(std::string urlOrClear) {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);

  std::string t = urlOrClear;
  // Normalize.
  while (!t.empty() && std::isspace(static_cast<unsigned char>(t.front()))) t.erase(t.begin());
  while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();

  if (t == "clear") t.clear();

  st.url = std::move(t);
  st.dirty = true;
  st.lastFetchOk = false;
  st.lastFetchUnixSec = 0;
  if (st.url.empty()) {
    st.admins.clear();
  } else {
    StartThreadIfNeededLocked(st);
  }
  st.cv.notify_one();
}

void ConfigureRefreshSeconds(int seconds) {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  st.refreshSeconds = ClampRefreshSeconds(seconds);
  st.dirty = true;
  StartThreadIfNeededLocked(st);
  st.cv.notify_one();
}

void SetBearerToken(std::optional<std::string> token) {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (token && token->empty()) token.reset();
  st.bearerToken = std::move(token);
  st.dirty = true;
  if (!st.url.empty()) StartThreadIfNeededLocked(st);
  st.cv.notify_one();
}

bool IsMatAdmin(uint64_t steamid64) {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  return st.admins.find(steamid64) != st.admins.end();
}

void RefreshNow() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  st.dirty = true;
  if (!st.url.empty()) StartThreadIfNeededLocked(st);
  st.cv.notify_one();
}

std::string AdminsUrl() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  return st.url;
}

int RefreshSeconds() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  return st.refreshSeconds;
}

bool LastFetchOk() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  return st.lastFetchOk;
}

}  // namespace readyup::mat_admins

