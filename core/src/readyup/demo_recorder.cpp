#include "readyup/demo_recorder.h"

#include "readyup/command_buffer_hook.h"
#include "readyup/game_timers.h"
#include "readyup/logging.h"
#include "readyup/match_stats.h"
#include "readyup/path.h"
#include "readyup/webhook.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <thread>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if !defined(READYUP_NO_CURL)
#include <curl/curl.h>
#endif

namespace readyup::demo {
namespace {

constexpr int kDefaultTvDelay = 10;  // CS2 default tv_delay

struct Recording {
  bool active = false;
  RecordingInfo info;
  std::string relPath;   // relative to csgo/, includes ".dem"
  std::string fileName;  // basename
  long long startedEpoch = 0;
};

struct State {
  std::mutex mu;
  Settings s;
  int observedTvDelay = -1;
  Recording rec;
  std::string lastUpload;
  std::vector<Listener> listeners;
};

State& St() {
  static State st;
  return st;
}

long long NowEpoch() { return static_cast<long long>(std::time(nullptr)); }

std::string LocalTimeToken() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M-%S", &tm);
  return buf;
}

std::string CsgoDir() {
  std::string d = GetCsgoDirFromModuleDir();
  while (!d.empty() && d.back() == '/') d.pop_back();
  return d;
}

void MkdirP(const std::string& dir) {
  std::string cur;
  for (size_t i = 0; i < dir.size(); ++i) {
    cur.push_back(dir[i]);
    if ((dir[i] == '/' || i + 1 == dir.size()) && cur.size() > 1) (void)mkdir(cur.c_str(), 0755);
  }
}

bool StatFile(const std::string& path, long long* size, long long* mtime) {
  struct stat sb {};
  if (stat(path.c_str(), &sb) != 0 || !S_ISREG(sb.st_mode)) return false;
  if (size) *size = static_cast<long long>(sb.st_size);
  if (mtime) *mtime = static_cast<long long>(sb.st_mtime);
  return true;
}

std::string Basename(const std::string& p) {
  const size_t s = p.find_last_of('/');
  return s == std::string::npos ? p : p.substr(s + 1);
}

std::string DirName(const std::string& p) {
  const size_t s = p.find_last_of('/');
  return s == std::string::npos ? std::string(".") : p.substr(0, s);
}

std::vector<DemoCandidate> ListDemos(const std::string& dir) {
  std::vector<DemoCandidate> out;
  DIR* d = opendir(dir.c_str());
  if (!d) return out;
  while (dirent* e = readdir(d)) {
    const std::string n = e->d_name;
    if (n.size() < 4 || n.compare(n.size() - 4, 4, ".dem") != 0) continue;
    long long mt = 0;
    if (StatFile(dir + "/" + n, nullptr, &mt)) out.push_back(DemoCandidate{dir + "/" + n, mt});
  }
  closedir(d);
  return out;
}

void Emit(const DemoEvent& e) {
  std::vector<Listener> ls;
  {
    std::lock_guard<std::mutex> lk(St().mu);
    ls = St().listeners;
  }
  Print("demo: %s\n", ToJson(e).c_str());
  for (auto& fn : ls) fn(e);
}

// ------------------------------------------------------------------------------- upload

struct PostResult {
  long status = 0;
  std::string body;
  std::string error;
};

#if !defined(READYUP_NO_CURL)
size_t ReadCb(char* buf, size_t size, size_t n, void* ud) { return std::fread(buf, size, n, static_cast<FILE*>(ud)); }

size_t WriteCb(char* ptr, size_t size, size_t n, void* ud) {
  auto* s = static_cast<std::string*>(ud);
  const size_t len = size * n;
  if (s->size() < 4096) s->append(ptr, std::min(len, 4096 - s->size()));
  return len;
}

const char* CaBundle() {
#if defined(READYUP_CURL_CA_PROBE)
  static const char* found = []() -> const char* {
    static const char* const kCandidates[] = {
        "/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt",
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", "/etc/ssl/ca-bundle.pem", "/etc/ssl/cert.pem"};
    for (const char* p : kCandidates) {
      if (access(p, R_OK) == 0) return p;
    }
    return nullptr;
  }();
  return found;
#else
  return nullptr;
#endif
}
#endif

PostResult SendFile(const std::string& method, const std::string& url, const std::string& path, long long size,
                    const std::vector<std::string>& headers) {
  PostResult r;
#if !defined(READYUP_NO_CURL)
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    r.error = "open failed";
    return r;
  }
  CURL* c = curl_easy_init();
  if (!c) {
    std::fclose(f);
    r.error = "curl_easy_init failed";
    return r;
  }
  curl_slist* hl = nullptr;
  hl = curl_slist_append(hl, "Content-Type: application/octet-stream");
  hl = curl_slist_append(hl, "Expect:");
  for (const auto& h : headers) hl = curl_slist_append(hl, h.c_str());
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  if (method == "PUT") {
    curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(c, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(size));
  } else {
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(size));
  }
  curl_easy_setopt(c, CURLOPT_READFUNCTION, &ReadCb);
  curl_easy_setopt(c, CURLOPT_READDATA, f);
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, &WriteCb);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
  // Big demos: no short total cap, give up when the transfer stalls.
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 1800L);
  if (const char* ca = CaBundle()) curl_easy_setopt(c, CURLOPT_CAINFO, ca);
  const CURLcode rc = curl_easy_perform(c);
  if (rc != CURLE_OK) r.error = curl_easy_strerror(rc);
  else curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
  curl_slist_free_all(hl);
  curl_easy_cleanup(c);
  std::fclose(f);
#else
  (void)size;
  std::vector<std::string> args = {"curl", "-sS", "-w", "\nREADYUP_STATUS:%{http_code}\n", "--connect-timeout", "5",
                                   "-m", "1800", "-X", method, "-H", "Content-Type: application/octet-stream",
                                   "-H", "Expect:"};
  for (const auto& h : headers) {
    args.emplace_back("-H");
    args.push_back(h);
  }
  args.emplace_back(method == "PUT" ? "-T" : "--data-binary");
  args.push_back(method == "PUT" ? path : "@" + path);
  args.push_back(url);
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);
  int fds[2];
  if (pipe(fds) != 0) {
    r.error = "pipe failed";
    return r;
  }
  const pid_t pid = fork();
  if (pid == 0) {
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[0]);
    close(fds[1]);
    execvp(argv[0], argv.data());
    _exit(127);
  }
  close(fds[1]);
  std::string out;
  char buf[4096];
  for (;;) {
    const ssize_t n = read(fds[0], buf, sizeof(buf));
    if (n > 0) {
      out.append(buf, static_cast<size_t>(n));
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    break;
  }
  close(fds[0]);
  int status = 0;
  if (pid > 0) waitpid(pid, &status, 0);
  const size_t p = out.rfind("\nREADYUP_STATUS:");
  if (p == std::string::npos) {
    r.error = "curl failed";
  } else {
    r.status = std::strtol(out.c_str() + p + 16, nullptr, 10);
    r.body = out.substr(0, std::min<size_t>(p, 4096));
    if (r.status == 0) r.error = "no response";
  }
#endif
  return r;
}

struct UploadJob {
  RecordingInfo info;
  int round = 0;
  int team1Score = 0;
  int team2Score = 0;
  std::string expectedPath;  // absolute
  std::string demoDir;       // absolute
  long long startedEpoch = 0;
  Settings s;
};

void RunUpload(UploadJob job) {
  // Let GOTV finish writing: a few seconds, then until the size settles.
  std::this_thread::sleep_for(std::chrono::seconds(3));
  std::string path = job.expectedPath;
  long long lastSize = -1;
  int stable = 0;
  for (int i = 0; i < 30; ++i) {
    long long sz = 0;
    if (StatFile(path, &sz, nullptr) && sz > 0 && sz == lastSize) {
      if (++stable >= 2) break;
    } else {
      stable = 0;
      lastSize = StatFile(path, &sz, nullptr) ? sz : -1;
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  if (!StatFile(path, nullptr, nullptr)) {
    std::vector<DemoCandidate> files;
    std::vector<std::string> seen;
    for (const auto& d : {DirName(job.expectedPath), job.demoDir, CsgoDir()}) {
      if (d.empty() || std::find(seen.begin(), seen.end(), d) != seen.end()) continue;
      seen.push_back(d);
      auto l = ListDemos(d);
      files.insert(files.end(), l.begin(), l.end());
    }
    const std::string found = PickDemoForMatch(files, Basename(job.expectedPath), job.info.matchid,
                                               job.info.mapName, job.startedEpoch > 0 ? job.startedEpoch - 120 : 0);
    if (!found.empty()) {
      Print("demo: expected file missing, using %s\n", found.c_str());
      path = found;
    }
  }

  DemoEvent ev;
  ev.matchid = job.info.matchid;
  ev.mapNumber = job.info.mapNumber;
  ev.fileName = Basename(path);
  ev.path = path;

  if (job.s.uploadUrl.empty()) {
    Print("demo: kept on disk (no ru_demo_upload_url): %s\n", path.c_str());
    return;
  }
  long long size = 0;
  if (!StatFile(path, &size, nullptr)) {
    ev.type = DemoEventType::UploadFailed;
    ev.error = "file_not_found";
    Print("demo: upload skipped, file not found (GOTV needs tv_enable 1 before the map loads): %s\n", path.c_str());
    Emit(ev);
    std::lock_guard<std::mutex> lk(St().mu);
    St().lastUpload = "file_not_found: " + ev.fileName;
    return;
  }
  ev.sizeMb = static_cast<double>(size) / 1024.0 / 1024.0;

  TokenValues tv;
  tv.matchid = job.info.matchid;
  tv.slug = job.info.slug;
  tv.map = job.info.mapName;
  tv.mapNumber = job.info.mapNumber;
  tv.team1 = job.info.team1;
  tv.team2 = job.info.team2;
  tv.team1Score = job.team1Score;
  tv.team2Score = job.team2Score;
  tv.fileName = ev.fileName;
  tv.roundNumber = job.round;
  const std::string url = ExpandTokens(job.s.uploadUrl, tv);
  std::vector<std::string> headers;
  for (const auto& h : job.s.uploadHeaders) headers.push_back(h.first + ": " + ExpandTokens(h.second, tv));

  ev.type = DemoEventType::UploadStarted;
  Emit(ev);

  PostResult r;
  const int attempts = std::max(1, job.s.uploadAttempts);
  int used = 0;
  for (int attempt = 1; attempt <= attempts; ++attempt) {
    used = attempt;
    r = SendFile(job.s.uploadMethod, url, path, size, headers);
    const bool transient = !r.error.empty() || r.status == 429 || r.status >= 500;
    if (!transient || attempt == attempts) break;
    Print("demo: upload attempt %d/%d failed (status=%ld %s); retrying\n", attempt, attempts, r.status,
          r.error.c_str());
    std::this_thread::sleep_for(std::chrono::seconds(attempt * attempt));
  }
  ev.attempts = used;
  ev.httpStatus = r.status;
  if (r.error.empty() && r.status >= 200 && r.status < 300) {
    ev.type = DemoEventType::UploadSucceeded;
  } else {
    ev.type = DemoEventType::UploadFailed;
    if (!r.error.empty()) ev.error = r.error;
    else ev.error = r.body.empty() ? "HTTP " + std::to_string(r.status) : r.body.substr(0, 100);
  }
  Emit(ev);
  std::lock_guard<std::mutex> lk(St().mu);
  St().lastUpload = std::string(ev.type == DemoEventType::UploadSucceeded ? "ok " : "failed ") +
                    std::to_string(r.status) + " " + ev.fileName + (ev.error.empty() ? "" : " (" + ev.error + ")");
}

std::string Trimmed(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  return s.substr(i);
}

}  // namespace

// ------------------------------------------------------------------------------ settings

Settings Get() {
  std::lock_guard<std::mutex> lk(St().mu);
  return St().s;
}

void SetRecordingEnabled(bool on) {
  std::lock_guard<std::mutex> lk(St().mu);
  St().s.recordingEnabled = on;
}

bool SetPath(const std::string& path, std::string* err) {
  if (!path.empty() && (path[0] == '/' || path[0] == '.' || path.back() != '/' ||
                        path.find("//") != std::string::npos || path.find("..") != std::string::npos)) {
    if (err) *err = "ru_demo_path must end with '/' and must not start with '/' or '.'";
    return false;
  }
  std::lock_guard<std::mutex> lk(St().mu);
  St().s.path = path;
  return true;
}

void SetNameFormat(const std::string& format) {
  if (format.empty()) return;
  std::lock_guard<std::mutex> lk(St().mu);
  St().s.nameFormat = format;
}

void SetUploadUrl(const std::string& url) {
  std::lock_guard<std::mutex> lk(St().mu);
  St().s.uploadUrl = url;
}

bool SetUploadMethod(const std::string& method) {
  std::string m = method;
  for (char& c : m) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  if (m != "POST" && m != "PUT") return false;
  std::lock_guard<std::mutex> lk(St().mu);
  St().s.uploadMethod = m;
  return true;
}

void SetUploadHeader(const std::string& name, const std::string& value) {
  const std::string n = Trimmed(name);
  if (n.empty() || n.find_first_of(":\r\n") != std::string::npos) return;
  if (value.find_first_of("\r\n") != std::string::npos) return;
  std::lock_guard<std::mutex> lk(St().mu);
  auto& hs = St().s.uploadHeaders;
  hs.erase(std::remove_if(hs.begin(), hs.end(), [&](const auto& h) { return h.first == n; }), hs.end());
  if (!value.empty()) hs.emplace_back(n, value);
}

void ClearUploadHeaders() {
  std::lock_guard<std::mutex> lk(St().mu);
  St().s.uploadHeaders.clear();
}

void SetUploadAttempts(int attempts) {
  std::lock_guard<std::mutex> lk(St().mu);
  St().s.uploadAttempts = std::max(1, std::min(10, attempts));
}

void ObserveTvDelay(int seconds) {
  std::lock_guard<std::mutex> lk(St().mu);
  St().observedTvDelay = std::max(0, seconds);
}

int TvDelaySeconds() {
  int fromCvars = -1;
  if (auto ctx = WebhookGetMatchContext()) {
    for (const char* k : {"tv_delay", "tv_delay1"}) {
      auto it = ctx->cvars.find(k);
      if (it != ctx->cvars.end()) fromCvars = std::max(fromCvars, std::atoi(it->second.c_str()));
    }
  }
  if (fromCvars >= 0) return fromCvars;
  std::lock_guard<std::mutex> lk(St().mu);
  return St().observedTvDelay >= 0 ? St().observedTvDelay : kDefaultTvDelay;
}

// ------------------------------------------------------------------------------ lifecycle

bool StartRecording(const RecordingInfo& info) {
  Settings s;
  {
    std::lock_guard<std::mutex> lk(St().mu);
    s = St().s;
    if (St().rec.active) return true;
  }
  if (!s.recordingEnabled) {
    PrintLine("demo: recording disabled (ru_demo_recording_enabled 0)");
    return false;
  }
  TokenValues tv;
  tv.time = LocalTimeToken();
  tv.matchid = info.matchid;
  tv.slug = info.slug;
  tv.map = info.mapName;
  tv.mapNumber = info.mapNumber;
  tv.team1 = info.team1;
  tv.team2 = info.team2;
  const std::string name = FormatDemoFileName(s.nameFormat, tv) + ".dem";
  const std::string rel = s.path + name;
  MkdirP(CsgoDir() + "/" + s.path);
  bool ok = EnqueueServerCommand("tv_enable 1");
  ok = EnqueueServerCommand(("tv_record \"" + rel + "\"").c_str()) && ok;
  if (!ok) {
    PrintLine("demo: command buffer unavailable; recording not started");
    return false;
  }
  {
    std::lock_guard<std::mutex> lk(St().mu);
    St().rec = Recording{true, info, rel, name, NowEpoch()};
  }
  DemoEvent e;
  e.type = DemoEventType::RecordingStarted;
  e.matchid = info.matchid;
  e.mapNumber = info.mapNumber;
  e.fileName = name;
  e.path = rel;
  Emit(e);
  return true;
}

bool IsRecording() {
  std::lock_guard<std::mutex> lk(St().mu);
  return St().rec.active;
}

bool StopAfterDelayAndUpload(double delaySeconds, int roundNumber, int team1Score, int team2Score) {
  Recording rec;
  {
    std::lock_guard<std::mutex> lk(St().mu);
    if (!St().rec.active) return false;
    rec = St().rec;
  }
  Print("demo: stopping %s in %.1fs (GOTV flush)\n", rec.relPath.c_str(), delaySeconds);
  ScheduleOnGameThread(delaySeconds, [rec, roundNumber, team1Score, team2Score]() {
    Settings s;
    {
      std::lock_guard<std::mutex> lk(St().mu);
      // A newer recording (next map) is not ours to stop.
      if (!St().rec.active || St().rec.relPath != rec.relPath) return;
      St().rec.active = false;
      s = St().s;
    }
    (void)EnqueueServerCommand("tv_stoprecord");
    DemoEvent e;
    e.type = DemoEventType::RecordingStopped;
    e.matchid = rec.info.matchid;
    e.mapNumber = rec.info.mapNumber;
    e.fileName = rec.fileName;
    e.path = rec.relPath;
    Emit(e);
    UploadJob job;
    job.info = rec.info;
    job.round = roundNumber;
    job.team1Score = team1Score;
    job.team2Score = team2Score;
    job.expectedPath = CsgoDir() + "/" + rec.relPath;
    job.demoDir = CsgoDir() + "/" + s.path;
    job.startedEpoch = rec.startedEpoch;
    job.s = s;
    std::thread(RunUpload, std::move(job)).detach();
  });
  return true;
}

void StopNowWithoutUpload() {
  Recording rec;
  {
    std::lock_guard<std::mutex> lk(St().mu);
    if (!St().rec.active) return;
    rec = St().rec;
    St().rec.active = false;
  }
  (void)EnqueueServerCommand("tv_stoprecord");
  DemoEvent e;
  e.type = DemoEventType::RecordingStopped;
  e.matchid = rec.info.matchid;
  e.mapNumber = rec.info.mapNumber;
  e.fileName = rec.fileName;
  e.path = rec.relPath;
  Emit(e);
}

void AddListener(Listener fn) {
  if (!fn) return;
  std::lock_guard<std::mutex> lk(St().mu);
  St().listeners.push_back(std::move(fn));
}

bool HandleConsoleLine(const std::vector<std::string>& args) {
  if (args.empty()) return false;
  const std::string& cmd = args[0];
  const bool hasVal = args.size() > 1;
  const std::string val = hasVal ? args[1] : std::string();

  if (cmd == "tv_delay") {
    if (hasVal) ObserveTvDelay(std::atoi(val.c_str()));
    return false;  // the engine still runs it
  }
  if (cmd == "ru_demo_recording_enabled") {
    if (hasVal) SetRecordingEnabled(val == "1" || val == "true" || val == "on");
    Print("ru_demo_recording_enabled = %d\n", Get().recordingEnabled ? 1 : 0);
    return true;
  }
  if (cmd == "ru_demo_path") {
    std::string err;
    if (hasVal && !SetPath(val, &err)) Print("%s\n", err.c_str());
    Print("ru_demo_path = \"%s\"\n", Get().path.c_str());
    return true;
  }
  if (cmd == "ru_demo_name_format") {
    if (hasVal) SetNameFormat(val);
    Print("ru_demo_name_format = \"%s\"\n", Get().nameFormat.c_str());
    return true;
  }
  if (cmd == "ru_demo_upload_url") {
    if (hasVal) SetUploadUrl(val == "clear" ? std::string() : val);
    PrintLine(Get().uploadUrl.empty() ? "ru_demo_upload_url: (not set)" : "ru_demo_upload_url: set");
    return true;
  }
  if (cmd == "ru_demo_upload_method") {
    if (hasVal && !SetUploadMethod(val)) PrintLine("ru_demo_upload_method: POST or PUT");
    Print("ru_demo_upload_method = %s\n", Get().uploadMethod.c_str());
    return true;
  }
  if (cmd == "ru_demo_upload_header") {
    if (args.size() < 2) {
      PrintLine("Usage: ru_demo_upload_header \"<Name>\" \"<value>\"  (empty value removes it)");
      return true;
    }
    SetUploadHeader(args[1], args.size() > 2 ? args[2] : std::string());
    Print("ru_demo_upload_header: %zu header(s) set\n", Get().uploadHeaders.size());
    return true;
  }
  if (cmd == "ru_demo_upload_headers_clear") {
    ClearUploadHeaders();
    PrintLine("ru_demo_upload_headers_clear: done");
    return true;
  }
  if (cmd == "ru_demo_upload_attempts") {
    if (hasVal) SetUploadAttempts(std::atoi(val.c_str()));
    Print("ru_demo_upload_attempts = %d\n", Get().uploadAttempts);
    return true;
  }
  if (cmd == "ru_demo_status") {
    for (const auto& l : StatusLines()) PrintLine(l.c_str());
    return true;
  }
  return false;
}

std::vector<std::string> StatusLines() {
  std::lock_guard<std::mutex> lk(St().mu);
  const auto& st = St();
  std::vector<std::string> out;
  out.push_back(std::string("demo: recording_enabled=") + (st.s.recordingEnabled ? "1" : "0") + " path=\"" +
                st.s.path + "\" name_format=\"" + st.s.nameFormat + "\"");
  std::string hdrs;
  for (const auto& h : st.s.uploadHeaders) hdrs += (hdrs.empty() ? "" : ",") + h.first;
  out.push_back("demo: upload " + st.s.uploadMethod + " " + (st.s.uploadUrl.empty() ? "(no url)" : "url set") +
                " attempts=" + std::to_string(st.s.uploadAttempts) + " headers=[" + hdrs + "]");
  out.push_back(std::string("demo: recording=") + (st.rec.active ? ("yes " + st.rec.relPath) : "no") +
                " tv_delay_seen=" + std::to_string(st.observedTvDelay));
  if (!st.lastUpload.empty()) out.push_back("demo: last upload: " + st.lastUpload);
  return out;
}

}  // namespace readyup::demo
