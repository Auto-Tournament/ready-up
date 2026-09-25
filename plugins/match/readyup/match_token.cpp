#include "readyup/match_token.h"

#include "readyup/mat_admins.h"
#include "readyup/webhook.h"

#include <mutex>

namespace readyup {
namespace {

std::mutex g_matchTokenMu;
std::string g_matchToken;

}  // namespace

std::optional<std::string> GetMatchTokenCopy() {
  std::lock_guard<std::mutex> lk(g_matchTokenMu);
  if (g_matchToken.empty()) return std::nullopt;
  return g_matchToken;
}

void SetMatchToken(std::string tokenOrEmpty) {
  std::lock_guard<std::mutex> lk(g_matchTokenMu);
  g_matchToken = std::move(tokenOrEmpty);
  if (g_matchToken.empty()) {
    WebhookSetBearerToken(std::nullopt);
    readyup::mat_admins::SetBearerToken(std::nullopt);
    readyup::mat_admins::RefreshNow();
  } else {
    WebhookSetBearerToken(g_matchToken);
    readyup::mat_admins::SetBearerToken(g_matchToken);
    readyup::mat_admins::RefreshNow();
  }
}

}  // namespace readyup

