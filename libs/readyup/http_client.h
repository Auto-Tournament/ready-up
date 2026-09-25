#pragma once

#include <optional>
#include <string>

namespace readyup {

struct HttpResponse {
  long status = 0;          // 0 if request did not complete
  std::string body;         // response body (may be empty)
  std::string error;        // curl error / failure reason (empty on success)
};

// Best-effort HTTP(S) GET.
// - Uses libcurl when available.
// - Applies short timeouts to avoid stalling the server thread for too long.
// - If bearerToken is set, sends: Authorization: Bearer <token>
// - Never logs tokens (caller controls logging).
HttpResponse HttpGet(std::string url, std::optional<std::string> bearerToken);

// Best-effort HTTP(S) POST with JSON body.
// - Uses libcurl when available.
// - Applies short timeouts to avoid stalling the server thread for too long.
// - If bearerToken is set, sends: Authorization: Bearer <token> and X-Auto-Tournament-Token: <token>
// - Sends Content-Type: application/json.
HttpResponse HttpPostJson(std::string url, std::optional<std::string> bearerToken, std::string jsonBody);

}  // namespace readyup

