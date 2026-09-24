#pragma once
// Local status endpoint (docs/FLEET.md §17): a minimal HTTP/1.1 request parser, response
// builder, per-IP token bucket and token check. Engine-free and allocation-light; the
// socket loop lives in status_server.cpp. Only GET and HEAD are ever served.
#include <cstddef>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace readyup::status {

struct HttpRequest {
  std::string method;
  std::string target;   // raw request target
  std::string path;     // percent-decoded path without the query
  std::string query;    // raw query (after '?'), may be empty
  std::string version;  // "HTTP/1.1"
  std::vector<std::pair<std::string, std::string>> headers;  // names lower-cased
  size_t headerBytes = 0;  // bytes consumed up to and including the blank line

  // First header with this (lower-case) name, or "".
  std::string Header(const std::string& lowerName) const;
  bool HasHeader(const std::string& lowerName) const;
  // Percent-decoded value of a query parameter, or "" (present-but-empty also yields "").
  std::string QueryParam(const std::string& name, bool* present = nullptr) const;
};

enum class ParseStatus {
  Incomplete,  // need more bytes
  Ok,
  Bad,         // malformed request line / header, or a body (we never accept one)
  TooLarge,    // headers exceed maxBytes
};

// Parses the request head in `buf` (request line + headers up to CRLFCRLF, bare LF accepted).
// Requests with a body (Content-Length > 0 or Transfer-Encoding) are Bad.
ParseStatus ParseRequest(const std::string& buf, size_t maxBytes, HttpRequest* out);

const char* HttpStatusText(int code);

// Full response (Connection: close). `headOnly` sends the headers (with the Content-Length
// of `body`) and no body.
std::string BuildResponse(int code, const std::string& contentType, const std::string& body, bool headOnly,
                          const std::vector<std::pair<std::string, std::string>>& extraHeaders = {});

// Response head for a Server-Sent Events stream (body follows until the socket closes).
std::string BuildSseHead();

// %XX decoding; '+' becomes a space only when plusIsSpace (query strings).
std::string PercentDecode(const std::string& s, bool plusIsSpace = false);

// Token from `Authorization: Bearer <t>`, `X-ReadyUp-Token: <t>` or `?token=<t>`.
std::string ExtractToken(const HttpRequest& req);
// Constant-time comparison (no early exit on the first differing byte).
bool TokenEquals(const std::string& a, const std::string& b);

// Per-key token bucket: `rate` tokens per second, up to `burst`. Keys (client IPs) that have
// been idle long enough to be full again are pruned when the table exceeds `maxKeys`.
// Game port from a NUL-separated /proc/self/cmdline (`-port N` or `+hostport N`), else 27015.
int GamePortFromCmdline(const std::string& nulSeparated);

class RateLimiter {
 public:
  RateLimiter(double rate, double burst, size_t maxKeys = 1024);
  bool Allow(const std::string& key, double nowSec);
  size_t Size() const { return buckets_.size(); }

 private:
  struct Bucket {
    double tokens = 0;
    double last = 0;
  };
  void Prune(double nowSec);
  double rate_, burst_;
  size_t maxKeys_;
  std::unordered_map<std::string, Bucket> buckets_;
};

}  // namespace readyup::status
