#include "readyup/workers.h"

#include "readyup/logging.h"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace readyup::workers {
namespace {

struct Entry {
  std::string what;
  std::shared_ptr<std::atomic<bool>> done;
  std::thread th;
};

std::mutex g_mu;  // guards g_threads
std::list<Entry> g_threads;
std::atomic<bool> g_stopping{false};
std::vector<std::function<void()>> g_wakers;  // guarded by g_mu

std::mutex g_sleepMu;
std::condition_variable g_sleepCv;
unsigned long long g_wakeGen = 0;  // guarded by g_sleepMu

// Must hold g_mu. Joins threads that already finished (they exit right away).
void ReapLocked() {
  for (auto it = g_threads.begin(); it != g_threads.end();) {
    if (it->done->load(std::memory_order_acquire)) {
      if (it->th.joinable()) it->th.join();
      it = g_threads.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace

bool Spawn(const char* what, std::function<void()> fn) {
  if (!fn || g_stopping.load(std::memory_order_acquire)) return false;
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_stopping.load(std::memory_order_acquire)) return false;
  ReapLocked();
  auto done = std::make_shared<std::atomic<bool>>(false);
  Entry e;
  e.what = what ? what : "worker";
  e.done = done;
  e.th = std::thread([fn = std::move(fn), done, name = e.what]() {
    try {
      fn();
    } catch (const std::exception& ex) {
      Print("match: worker %s threw: %s\n", name.c_str(), ex.what());
    } catch (...) {
      Print("match: worker %s threw\n", name.c_str());
    }
    done->store(true, std::memory_order_release);
  });
  g_threads.push_back(std::move(e));
  return true;
}

bool ShuttingDown() { return g_stopping.load(std::memory_order_acquire); }

bool SleepFor(std::chrono::milliseconds d) {
  std::unique_lock<std::mutex> lk(g_sleepMu);
  const unsigned long long gen = g_wakeGen;
  g_sleepCv.wait_for(lk, d, [&] { return g_stopping.load(std::memory_order_acquire) || g_wakeGen != gen; });
  return !g_stopping.load(std::memory_order_acquire);
}

void WakeAll() {
  {
    std::lock_guard<std::mutex> lk(g_sleepMu);
    ++g_wakeGen;
  }
  g_sleepCv.notify_all();
}

void AddWaker(std::function<void()> fn) {
  if (!fn) return;
  std::lock_guard<std::mutex> lk(g_mu);
  g_wakers.push_back(std::move(fn));
}

void Shutdown() {
  g_stopping.store(true, std::memory_order_release);
  WakeAll();
  std::list<Entry> all;
  std::vector<std::function<void()>> wakers;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    all.swap(g_threads);
    wakers.swap(g_wakers);
  }
  for (auto& w : wakers) w();
  const auto t0 = std::chrono::steady_clock::now();
  for (auto& e : all) {
    const auto t1 = std::chrono::steady_clock::now();
    if (e.th.joinable()) e.th.join();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t1).count();
    if (ms > 500) Print("match: unload waited %lld ms for worker %s\n", static_cast<long long>(ms), e.what.c_str());
  }
  const auto total =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  if (!all.empty()) Debug("match: joined %zu worker thread(s) in %lld ms\n", all.size(), static_cast<long long>(total));
}

void Start() {
  g_stopping.store(false, std::memory_order_release);
  // The core never unloads plugins on quit, so exit() would destroy g_threads (joinable threads:
  // std::terminate) and g_sleepCv (a worker still waits on it: hang). A handler registered here
  // runs before the destructors of this image's statics, which registered earlier. atexit() from
  // a shared object is tied to its __dso_handle, so dlclose() on reload drops it.
  static bool registered = false;
  if (!registered) {
    registered = true;
    std::atexit([] { Shutdown(); });
  }
}

}  // namespace readyup::workers
