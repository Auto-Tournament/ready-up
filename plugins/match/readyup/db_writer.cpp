#include "readyup/db_writer.h"

#include "readyup/logging.h"
#include "readyup/postgres.h"
#include "readyup/workers.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <utility>

namespace readyup::db_writer {
namespace {

struct Job {
  std::string key;
  std::optional<std::string> value;
};

std::mutex g_mu;
std::condition_variable g_cv;
std::deque<Job> g_jobs;
bool g_running = false;  // worker alive (guarded by g_mu)
constexpr size_t kMaxJobs = 1024;

void Worker() {
  bool schemaOk = false;
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lk(g_mu);
      g_cv.wait(lk, [] { return !g_jobs.empty() || workers::ShuttingDown(); });
      if (g_jobs.empty()) {  // shutting down, nothing left
        g_running = false;
        return;
      }
      job = std::move(g_jobs.front());
      g_jobs.pop_front();
    }
    std::string err;
    if (!schemaOk) schemaOk = pg::EnsureSchema(&err);
    if (!schemaOk) {
      Debug("db-writer: schema unavailable (%s); dropping write of %s\n", err.c_str(), job.key.c_str());
      continue;
    }
    err.clear();
    const bool ok = job.value ? pg::SetSetting(job.key, *job.value, &err) : pg::ClearSetting(job.key, &err);
    if (!ok) Debug("db-writer: %s %s failed: %s\n", job.value ? "set" : "clear", job.key.c_str(), err.c_str());
  }
}

}  // namespace

void SetSettingAsync(std::string key, std::optional<std::string> value) {
  if (!pg::Available() || key.empty()) return;
  std::unique_lock<std::mutex> lk(g_mu);
  if (g_jobs.size() >= kMaxJobs) g_jobs.pop_front();
  g_jobs.push_back(Job{std::move(key), std::move(value)});
  if (!g_running) {
    static bool s_wakerAdded = false;
    if (!s_wakerAdded) {
      s_wakerAdded = true;
      workers::AddWaker([] {
        std::lock_guard<std::mutex> l(g_mu);
        g_cv.notify_all();
      });
    }
    g_running = workers::Spawn("db-writer", Worker);
    if (!g_running) {
      g_jobs.clear();  // unloading: nobody would write it
      return;
    }
  }
  lk.unlock();
  g_cv.notify_one();
}

}  // namespace readyup::db_writer
