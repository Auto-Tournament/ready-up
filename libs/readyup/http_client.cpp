#include "readyup/http_client.h"

#include "readyup/config.h"
#include "readyup/logging.h"

#include <mutex>
#include <string>
#include <vector>

#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if !defined(READYUP_NO_CURL)
#include <curl/curl.h>
#endif

namespace readyup {
namespace {

constexpr long kConnectTimeoutMs = 3000;
constexpr long kTotalTimeoutMs = 8000;
constexpr long kMaxRedirects = 5;

#if !defined(READYUP_NO_CURL)
static size_t WriteCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  if (!ptr || !userdata) return 0;
  const size_t n = size * nmemb;
  auto* s = reinterpret_cast<std::string*>(userdata);
  s->append(ptr, n);
  return n;
}

static void CurlGlobalInitOnce() {
  static std::once_flag once;
  std::call_once(once, []() {
    const CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (rc != CURLE_OK) {
      // Can't use Ready Up logging safely if curl isn't initialized; stderr is OK.
      // But we still keep it quiet unless debug is enabled.
      if (DebugEnabled()) {
        Print("http: curl_global_init failed: %s\n", curl_easy_strerror(rc));
      }
    }
  });
}

#if defined(READYUP_CURL_CA_PROBE)
// Release builds link a static libcurl/OpenSSL whose compiled-in CA bundle path is
// the Debian one. Point curl at whichever system bundle exists on this host.
static const char* SystemCaBundle() {
  static const char* found = []() -> const char* {
    static const char* const kCandidates[] = {
        "/etc/ssl/certs/ca-certificates.crt",                 // Debian/Ubuntu/Arch/Gentoo
        "/etc/pki/tls/certs/ca-bundle.crt",                   // Fedora/RHEL/CentOS
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",  // RHEL 7+
        "/etc/ssl/ca-bundle.pem",                             // openSUSE
        "/etc/ssl/cert.pem",                                  // Alpine/macOS-style
    };
    for (const char* p : kCandidates) {
      if (access(p, R_OK) == 0) return p;
    }
    return nullptr;
  }();
  return found;
}
#endif

static void ApplyCaBundle(CURL* curl) {
#if defined(READYUP_CURL_CA_PROBE)
  if (const char* ca = SystemCaBundle()) curl_easy_setopt(curl, CURLOPT_CAINFO, ca);
#else
  (void)curl;
#endif
}
#endif

static std::string TrimWs(std::string s) {
  auto is_ws = [](unsigned char ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; };
  while (!s.empty() && is_ws(static_cast<unsigned char>(s.back()))) s.pop_back();
  size_t i = 0;
  while (i < s.size() && is_ws(static_cast<unsigned char>(s[i]))) ++i;
  if (i) s.erase(0, i);
  return s;
}

static bool LooksLikeHttpUrl(const std::string& url) {
  if (url.rfind("http://", 0) == 0) return true;
  if (url.rfind("https://", 0) == 0) return true;
  return false;
}

}  // namespace

HttpResponse HttpGet(std::string url, std::optional<std::string> bearerToken) {
  HttpResponse out;
  url = TrimWs(std::move(url));
  if (url.empty()) {
    out.error = "empty url";
    return out;
  }
  if (!LooksLikeHttpUrl(url)) {
    out.error = "url must start with http:// or https://";
    return out;
  }

#if defined(READYUP_NO_CURL)
  // Fallback: call `curl` via fork/exec (no shell). This keeps the feature usable on
  // systems without libcurl dev packages installed.
  //
  // NOTE: Requires `curl` binary to be present in PATH at runtime.
  std::vector<std::string> args;
  args.emplace_back("curl");
  args.emplace_back("-sS");  // silent but show errors
  args.emplace_back("-L");   // follow redirects
  args.emplace_back("--connect-timeout");
  args.emplace_back(std::to_string(kConnectTimeoutMs / 1000));
  args.emplace_back("-m");
  args.emplace_back(std::to_string(kTotalTimeoutMs / 1000));

  // Append status marker to stdout so we can parse it.
  args.emplace_back("-w");
  args.emplace_back("\nREADYUP_STATUS:%{http_code}\n");

  if (bearerToken && !bearerToken->empty()) {
    args.emplace_back("-H");
    args.emplace_back("Authorization: Bearer " + *bearerToken);
  }

  args.emplace_back(url);

  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (auto& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);

  int pipefd[2];
  if (pipe(pipefd) != 0) {
    out.error = "pipe() failed";
    return out;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    out.error = "fork() failed";
    return out;
  }

  if (pid == 0) {
    // child
    (void)dup2(pipefd[1], STDOUT_FILENO);
    (void)dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[0]);
    close(pipefd[1]);
    execvp(argv[0], argv.data());
    _exit(127);
  }

  // parent
  close(pipefd[1]);
  std::string buf;
  char tmp[4096];
  for (;;) {
    const ssize_t n = read(pipefd[0], tmp, sizeof(tmp));
    if (n > 0) {
      buf.append(tmp, static_cast<size_t>(n));
      // Keep this bounded (body printing is truncated anyway).
      if (buf.size() > 1024 * 1024) break;
      continue;
    }
    if (n == 0) break;
    if (errno == EINTR) continue;
    break;
  }
  close(pipefd[0]);

  int status = 0;
  (void)waitpid(pid, &status, 0);

  // Parse HTTP status marker from the last occurrence.
  const std::string needle = "\nREADYUP_STATUS:";
  const size_t pos = buf.rfind(needle);
  if (pos == std::string::npos) {
    out.body = std::move(buf);
    out.error = "curl output missing status marker";
    return out;
  }

  out.body = buf.substr(0, pos);  // body + stderr (if any) before marker
  size_t codeStart = pos + needle.size();
  size_t codeEnd = buf.find_first_of("\r\n", codeStart);
  std::string codeStr = (codeEnd == std::string::npos) ? buf.substr(codeStart) : buf.substr(codeStart, codeEnd - codeStart);

  char* endp = nullptr;
  long code = std::strtol(codeStr.c_str(), &endp, 10);
  if (endp == codeStr.c_str()) {
    out.error = "failed to parse http status from curl output";
    return out;
  }
  out.status = code;

  // If curl failed, surface that too.
  if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
    out.error = "curl failed";
  }
  return out;
#else
  CurlGlobalInitOnce();

  CURL* curl = curl_easy_init();
  if (!curl) {
    out.error = "curl_easy_init failed";
    return out;
  }

  std::string body;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, kMaxRedirects);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, kConnectTimeoutMs);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kTotalTimeoutMs);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &WriteCb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);

  // Allow gzip/deflate/br if libcurl supports it.
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

  // Verify TLS by default (explicit for clarity).
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  ApplyCaBundle(curl);

  // UA helps debugging on proxies/CDNs.
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "ReadyUp/1 (ru match load)");

  struct curl_slist* headers = nullptr;
  if (bearerToken && !bearerToken->empty()) {
    // Never log the token. Just attach the header.
    const std::string h = "Authorization: Bearer " + *bearerToken;
    headers = curl_slist_append(headers, h.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  }

  CURLcode rc = curl_easy_perform(curl);
  long status = 0;
  (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

  if (headers) curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  out.status = status;
  out.body = std::move(body);
  if (rc != CURLE_OK) {
    out.error = curl_easy_strerror(rc);
  }
  return out;
#endif
}

HttpResponse HttpPostJson(std::string url, std::optional<std::string> bearerToken, std::string jsonBody) {
  HttpResponse out;
  url = TrimWs(std::move(url));
  if (url.empty()) {
    out.error = "empty url";
    return out;
  }
  if (!LooksLikeHttpUrl(url)) {
    out.error = "url must start with http:// or https://";
    return out;
  }

#if defined(READYUP_NO_CURL)
  std::vector<std::string> args;
  args.emplace_back("curl");
  args.emplace_back("-sS");
  args.emplace_back("-L");
  args.emplace_back("--connect-timeout");
  args.emplace_back(std::to_string(kConnectTimeoutMs / 1000));
  args.emplace_back("-m");
  args.emplace_back(std::to_string(kTotalTimeoutMs / 1000));

  args.emplace_back("-X");
  args.emplace_back("POST");
  args.emplace_back("-H");
  args.emplace_back("Content-Type: application/json");

  if (bearerToken && !bearerToken->empty()) {
    args.emplace_back("-H");
    args.emplace_back("Authorization: Bearer " + *bearerToken);
    // The Auto Tournament platform authenticates events with this header (same token).
    args.emplace_back("-H");
    args.emplace_back("X-Auto-Tournament-Token: " + *bearerToken);
  }

  // Use --data-binary to avoid curl mangling.
  args.emplace_back("--data-binary");
  args.emplace_back(jsonBody);

  // Append status marker to stdout so we can parse it.
  args.emplace_back("-w");
  args.emplace_back("\nREADYUP_STATUS:%{http_code}\n");

  args.emplace_back(url);

  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (auto& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);

  int pipefd[2];
  if (pipe(pipefd) != 0) {
    out.error = "pipe() failed";
    return out;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    out.error = "fork() failed";
    return out;
  }

  if (pid == 0) {
    (void)dup2(pipefd[1], STDOUT_FILENO);
    (void)dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[0]);
    close(pipefd[1]);
    execvp(argv[0], argv.data());
    _exit(127);
  }

  close(pipefd[1]);
  std::string buf;
  char tmp[4096];
  for (;;) {
    const ssize_t n = read(pipefd[0], tmp, sizeof(tmp));
    if (n > 0) {
      buf.append(tmp, static_cast<size_t>(n));
      if (buf.size() > 1024 * 1024) break;
      continue;
    }
    if (n == 0) break;
    if (errno == EINTR) continue;
    break;
  }
  close(pipefd[0]);

  int status = 0;
  (void)waitpid(pid, &status, 0);

  const std::string needle = "\nREADYUP_STATUS:";
  const size_t pos = buf.rfind(needle);
  if (pos == std::string::npos) {
    out.body = std::move(buf);
    out.error = "curl output missing status marker";
    return out;
  }

  out.body = buf.substr(0, pos);
  size_t codeStart = pos + needle.size();
  size_t codeEnd = buf.find_first_of("\r\n", codeStart);
  std::string codeStr = (codeEnd == std::string::npos) ? buf.substr(codeStart) : buf.substr(codeStart, codeEnd - codeStart);

  char* endp = nullptr;
  long code = std::strtol(codeStr.c_str(), &endp, 10);
  if (endp == codeStr.c_str()) {
    out.error = "failed to parse http status from curl output";
    return out;
  }
  out.status = code;

  if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
    out.error = "curl failed";
  }
  return out;
#else
  CurlGlobalInitOnce();

  CURL* curl = curl_easy_init();
  if (!curl) {
    out.error = "curl_easy_init failed";
    return out;
  }

  std::string body;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, kMaxRedirects);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, kConnectTimeoutMs);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kTotalTimeoutMs);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &WriteCb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  ApplyCaBundle(curl);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "ReadyUp/1 (webhook)");

  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonBody.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(jsonBody.size()));

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  if (bearerToken && !bearerToken->empty()) {
    const std::string h = "Authorization: Bearer " + *bearerToken;
    headers = curl_slist_append(headers, h.c_str());
    // The Auto Tournament platform authenticates events with this header (same token).
    const std::string at = "X-Auto-Tournament-Token: " + *bearerToken;
    headers = curl_slist_append(headers, at.c_str());
  }
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

  CURLcode rc = curl_easy_perform(curl);
  long status = 0;
  (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

  out.status = status;
  out.body = std::move(body);

  if (rc != CURLE_OK) {
    out.error = curl_easy_strerror(rc);
  }

  if (headers) curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return out;
#endif
}

}  // namespace readyup

