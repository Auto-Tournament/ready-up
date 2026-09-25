#include "readyup/match_signals.h"

#include <atomic>
#include <mutex>

namespace readyup::signals {
namespace {

std::mutex g_mu;
std::vector<Signal> g_queue;
std::atomic<bool> g_enabled{false};
constexpr size_t kMaxQueued = 4096;  // a stalled game thread must not grow this forever

}  // namespace

void Emit(const char* type, status::Json data, int round, int mapNumber) {
  if (!g_enabled.load(std::memory_order_relaxed) || !type) return;
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_queue.size() >= kMaxQueued) return;
  g_queue.push_back(Signal{type, std::move(data), round, mapNumber});
}

void SetEnabled(bool on) {
  g_enabled.store(on);
  if (!on) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_queue.clear();
  }
}

bool Enabled() { return g_enabled.load(); }

std::vector<Signal> Take() {
  std::vector<Signal> out;
  std::lock_guard<std::mutex> lk(g_mu);
  out.swap(g_queue);
  return out;
}

}  // namespace readyup::signals
