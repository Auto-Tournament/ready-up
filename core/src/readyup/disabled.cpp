#include "readyup/disabled.h"

#include <mutex>

namespace readyup {
namespace {

std::mutex g_mu;
bool g_disabled = false;
std::string g_reason;

}  // namespace

bool IsDisabled() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_disabled;
}

std::string DisabledReason() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_reason;
}

void Disable(std::string reason) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_disabled = true;
  g_reason = std::move(reason);
}

}  // namespace readyup

