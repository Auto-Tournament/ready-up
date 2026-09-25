#include "readyup/game_timers.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <mutex>
#include <vector>

namespace readyup {
namespace {

using Clock = std::chrono::steady_clock;

struct Timer {
  Clock::time_point due;
  unsigned long long seq = 0;
  std::function<void()> fn;
};

std::mutex g_mu;
std::vector<Timer> g_timers;
unsigned long long g_seq = 0;

}  // namespace

void ScheduleOnGameThread(double seconds, std::function<void()> fn) {
  if (!fn) return;
  const auto delay = std::chrono::milliseconds(static_cast<long long>(std::max(0.0, seconds) * 1000.0));
  std::lock_guard<std::mutex> lk(g_mu);
  g_timers.push_back(Timer{Clock::now() + delay, ++g_seq, std::move(fn)});
}

void GameTimersFrameTick() {
  std::vector<Timer> due;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_timers.empty()) return;
    const auto now = Clock::now();
    auto split = std::partition(g_timers.begin(), g_timers.end(), [&](const Timer& t) { return t.due > now; });
    std::move(split, g_timers.end(), std::back_inserter(due));
    g_timers.erase(split, g_timers.end());
  }
  std::sort(due.begin(), due.end(),
            [](const Timer& a, const Timer& b) { return a.due != b.due ? a.due < b.due : a.seq < b.seq; });
  for (auto& t : due) t.fn();
}

}  // namespace readyup
