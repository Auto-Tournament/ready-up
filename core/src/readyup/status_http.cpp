#include "readyup/status_http.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace readyup::status {
namespace {

std::string Lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string TrimOws(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
  return s.substr(a, b - a);
}

bool IsTchar(unsigned char c) {
  if (std::isalnum(c)) return true;
  switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+': case '-': case '.':
    case '^': case '_': case '`': case '|': case '~':
      return true;
    default:
      return false;
  }
}

int HexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

std::string HttpRequest::Header(const std::string& lowerName) const {
  for (const auto& h : headers) {
    if (h.first == lowerName) return h.second;
  }
  return {};
}

bool HttpRequest::HasHeader(const std::string& lowerName) const {
  for (const auto& h : headers) {
    if (h.first == lowerName) return true;
  }
  return false;
}

std::string HttpRequest::QueryParam(const std::string& name, bool* present) const {
  if (present) *present = false;
  size_t pos = 0;
  while (pos <= query.size()) {
    size_t amp = query.find('&', pos);
    if (amp == std::string::npos) amp = query.size();
    const std::string part = query.substr(pos, amp - pos);
    const size_t eq = part.find('=');
    const std::string k = PercentDecode(part.substr(0, eq), true);
    if (k == name) {
      if (present) *present = true;
      return eq == std::string::npos ? std::string() : PercentDecode(part.substr(eq + 1), true);
    }
    pos = amp + 1;
  }
  return {};
}

std::string PercentDecode(const std::string& s, bool plusIsSpace) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '%' && i + 2 < s.size()) {
      const int hi = HexVal(s[i + 1]);
      const int lo = HexVal(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out += static_cast<char>(hi * 16 + lo);
        i += 2;
        continue;
      }
    }
    out += (plusIsSpace && c == '+') ? ' ' : c;
  }
  return out;
}

ParseStatus ParseRequest(const std::string& buf, size_t maxBytes, HttpRequest* out) {
  // Find the end of the head: CRLFCRLF, or LFLF from lenient clients.
  size_t end = std::string::npos, sepLen = 0;
  const size_t crlf = buf.find("\r\n\r\n");
  const size_t lflf = buf.find("\n\n");
  if (crlf != std::string::npos && (lflf == std::string::npos || crlf < lflf)) {
    end = crlf;
    sepLen = 4;
  } else if (lflf != std::string::npos) {
    end = lflf;
    sepLen = 2;
  }
  if (end == std::string::npos) {
    return buf.size() >= maxBytes ? ParseStatus::TooLarge : ParseStatus::Incomplete;
  }
  if (end + sepLen > maxBytes) return ParseStatus::TooLarge;

  HttpRequest r;
  r.headerBytes = end + sepLen;
  const std::string head = buf.substr(0, end);

  // Split into lines.
  std::vector<std::string> lines;
  size_t pos = 0;
  while (pos <= head.size()) {
    size_t nl = head.find('\n', pos);
    if (nl == std::string::npos) nl = head.size();
    std::string line = head.substr(pos, nl - pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(std::move(line));
    pos = nl + 1;
  }
  // Tolerate leading empty lines (RFC 9112 §2.2).
  size_t li = 0;
  while (li < lines.size() && lines[li].empty()) ++li;
  if (li >= lines.size()) return ParseStatus::Bad;

  // Request line: METHOD SP target SP HTTP/x.y
  const std::string& rl = lines[li++];
  const size_t sp1 = rl.find(' ');
  if (sp1 == std::string::npos || sp1 == 0) return ParseStatus::Bad;
  const size_t sp2 = rl.find(' ', sp1 + 1);
  if (sp2 == std::string::npos || sp2 == sp1 + 1) return ParseStatus::Bad;
  if (rl.find(' ', sp2 + 1) != std::string::npos) return ParseStatus::Bad;
  r.method = rl.substr(0, sp1);
  r.target = rl.substr(sp1 + 1, sp2 - sp1 - 1);
  r.version = rl.substr(sp2 + 1);
  for (unsigned char c : r.method) {
    if (!IsTchar(c)) return ParseStatus::Bad;
  }
  if (r.version != "HTTP/1.1" && r.version != "HTTP/1.0") return ParseStatus::Bad;
  for (unsigned char c : r.target) {
    if (c <= 0x20 || c == 0x7f) return ParseStatus::Bad;
  }
  // origin-form only; absolute-form "http://host/path" is accepted by taking the path.
  std::string target = r.target;
  if (target.rfind("http://", 0) == 0 || target.rfind("https://", 0) == 0) {
    const size_t slash = target.find('/', target.find("//") + 2);
    target = slash == std::string::npos ? "/" : target.substr(slash);
  }
  if (target.empty() || (target[0] != '/' && target != "*")) return ParseStatus::Bad;
  const size_t q = target.find('?');
  const size_t hash = target.find('#');
  std::string rawPath = target.substr(0, std::min(q, hash));
  if (q != std::string::npos) r.query = target.substr(q + 1, hash == std::string::npos ? std::string::npos : hash - q - 1);
  r.path = PercentDecode(rawPath);
  // Normalize a trailing slash ("/status/" == "/status").
  while (r.path.size() > 1 && r.path.back() == '/') r.path.pop_back();

  for (; li < lines.size(); ++li) {
    const std::string& l = lines[li];
    if (l.empty()) continue;
    if (l[0] == ' ' || l[0] == '\t') return ParseStatus::Bad;  // obsolete line folding
    const size_t colon = l.find(':');
    if (colon == std::string::npos || colon == 0) return ParseStatus::Bad;
    const std::string name = l.substr(0, colon);
    for (unsigned char c : name) {
      if (!IsTchar(c)) return ParseStatus::Bad;
    }
    if (r.headers.size() >= 100) return ParseStatus::TooLarge;
    r.headers.emplace_back(Lower(name), TrimOws(l.substr(colon + 1)));
  }

  if (r.HasHeader("transfer-encoding")) return ParseStatus::Bad;
  if (r.HasHeader("content-length")) {
    const std::string cl = r.Header("content-length");
    if (cl.empty() || cl.find_first_not_of("0123456789") != std::string::npos) return ParseStatus::Bad;
    if (std::strtoull(cl.c_str(), nullptr, 10) != 0) return ParseStatus::Bad;
  }
  if (r.version == "HTTP/1.1" && !r.HasHeader("host")) return ParseStatus::Bad;

  if (out) *out = std::move(r);
  return ParseStatus::Ok;
}

const char* HttpStatusText(int code) {
  switch (code) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "Unknown";
  }
}

std::string BuildResponse(int code, const std::string& contentType, const std::string& body, bool headOnly,
                          const std::vector<std::pair<std::string, std::string>>& extraHeaders) {
  std::string r;
  r.reserve(body.size() + 256);
  r += "HTTP/1.1 " + std::to_string(code) + " " + HttpStatusText(code) + "\r\n";
  if (!contentType.empty()) r += "Content-Type: " + contentType + "\r\n";
  r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  r += "Cache-Control: no-store\r\n";
  r += "X-Content-Type-Options: nosniff\r\n";
  r += "Connection: close\r\n";
  for (const auto& h : extraHeaders) r += h.first + ": " + h.second + "\r\n";
  r += "\r\n";
  if (!headOnly) r += body;
  return r;
}

std::string BuildSseHead() {
  return "HTTP/1.1 200 OK\r\n"
         "Content-Type: text/event-stream; charset=utf-8\r\n"
         "Cache-Control: no-store\r\n"
         "X-Accel-Buffering: no\r\n"
         "X-Content-Type-Options: nosniff\r\n"
         "Connection: close\r\n"
         "\r\n";
}

std::string ExtractToken(const HttpRequest& req) {
  const std::string auth = req.Header("authorization");
  if (auth.size() > 7) {
    std::string scheme = Lower(auth.substr(0, 7));
    if (scheme == "bearer ") return TrimOws(auth.substr(7));
  }
  const std::string x = req.Header("x-readyup-token");
  if (!x.empty()) return x;
  return req.QueryParam("token");
}

bool TokenEquals(const std::string& a, const std::string& b) {
  // Length is not secret (tokens have a fixed format); the content is.
  const size_t n = std::max(a.size(), b.size());
  unsigned char diff = a.size() == b.size() ? 0 : 1;
  for (size_t i = 0; i < n; ++i) {
    const unsigned char x = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
    const unsigned char y = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
    diff |= static_cast<unsigned char>(x ^ y);
  }
  return diff == 0 && !a.empty();
}

int GamePortFromCmdline(const std::string& nulSeparated) {
  std::vector<std::string> args;
  size_t pos = 0;
  while (pos < nulSeparated.size()) {
    size_t z = nulSeparated.find('\0', pos);
    if (z == std::string::npos) z = nulSeparated.size();
    args.push_back(nulSeparated.substr(pos, z - pos));
    pos = z + 1;
  }
  int port = 0;
  for (size_t i = 0; i + 1 < args.size(); ++i) {
    if (args[i] == "-port" || args[i] == "+hostport") {
      const long v = std::strtol(args[i + 1].c_str(), nullptr, 10);
      if (v > 0 && v < 65536) port = static_cast<int>(v);  // last one wins, like the engine
    }
  }
  return port ? port : 27015;
}

RateLimiter::RateLimiter(double rate, double burst, size_t maxKeys)
    : rate_(rate > 0 ? rate : 1), burst_(burst >= 1 ? burst : 1), maxKeys_(maxKeys ? maxKeys : 1) {}

void RateLimiter::Prune(double nowSec) {
  const double fullAfter = burst_ / rate_;
  for (auto it = buckets_.begin(); it != buckets_.end();) {
    if (nowSec - it->second.last >= fullAfter) it = buckets_.erase(it);
    else ++it;
  }
}

bool RateLimiter::Allow(const std::string& key, double nowSec) {
  auto it = buckets_.find(key);
  if (it == buckets_.end()) {
    if (buckets_.size() >= maxKeys_) {
      Prune(nowSec);
      // Still full (a flood of distinct sources): refuse new keys rather than grow.
      if (buckets_.size() >= maxKeys_) return false;
    }
    it = buckets_.emplace(key, Bucket{burst_, nowSec}).first;
  }
  Bucket& b = it->second;
  if (nowSec > b.last) {
    b.tokens = std::min(burst_, b.tokens + (nowSec - b.last) * rate_);
    b.last = nowSec;
  }
  if (b.tokens < 1.0) return false;
  b.tokens -= 1.0;
  return true;
}

}  // namespace readyup::status
