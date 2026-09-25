#include "readyup/perf_stats.h"

#include <algorithm>
#include <cstdio>

namespace readyup {

namespace {

std::string Ms(double us) {
  char b[32];
  std::snprintf(b, sizeof b, "%.2f ms", us / 1000.0);
  return b;
}

}  // namespace

void PerfStats::Add(const std::string& plugin, const std::string& what, double us) {
  if (us < 0) us = 0;
  const std::string key = plugin + " " + what;
  auto it = rows_.find(key);
  if (it == rows_.end()) {
    // Command and event names are open-ended: past the cap, fold new ones into "<plugin> other".
    const std::string k = rows_.size() < kMaxRows ? key : plugin + " other";
    it = rows_.emplace(k, Row{}).first;
  }
  Row& r = it->second;
  ++r.calls;
  r.totalUs += us;
  r.maxUs = std::max(r.maxUs, us);
  thisFrame_[it->first] += us;
  frameUs_ += us;
}

std::string PerfStats::EndFrame(double nowS, double warnMs, double gapWarnMs, double minLogGapS) {
  ++frames_;
  const double gapMs = lastFrameS_ < 0 ? 0 : (nowS - lastFrameS_) * 1000.0;
  lastFrameS_ = nowS;
  maxFrameUs_ = std::max(maxFrameUs_, frameUs_);
  maxGapMs_ = std::max(maxGapMs_, gapMs);
  const bool slow = frameUs_ > warnMs * 1000.0;
  const bool gap = gapWarnMs > 0 && gapMs > gapWarnMs;
  if (slow) ++slowFrames_;
  if (gap) ++gaps_;

  std::string line;
  if ((slow || gap) && nowS - lastLogS_ >= minLogGapS) {
    lastLogS_ = nowS;
    std::vector<std::pair<double, std::string>> top;
    for (const auto& kv : thisFrame_) top.emplace_back(kv.second, kv.first);
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    char head[160];
    std::snprintf(head, sizeof head, "perf: %s frame: %.2f ms in plugins, %.0f ms since the last frame",
                  slow ? "slow" : "late", frameUs_ / 1000.0, gapMs);
    line = head;
    if (!slow && gap) line += " (Ready Up was not the cause: the engine itself, a map load or hibernation)";
    for (size_t i = 0; i < top.size() && i < 3; ++i) line += (i ? ", " : "; top: ") + top[i].second + " " + Ms(top[i].first);
  }
  thisFrame_.clear();
  frameUs_ = 0;
  return line;
}

std::vector<std::string> PerfStats::Report(double nowS, size_t maxRows) const {
  std::vector<std::string> out;
  char b[200];
  std::snprintf(b, sizeof b,
                "perf: %.0f s, %llu frames; plugins' worst frame %.2f ms, %llu over the warn level; "
                "longest gap between frames %.0f ms (%llu over)",
                nowS - sinceS_, static_cast<unsigned long long>(frames_), maxFrameUs_ / 1000.0,
                static_cast<unsigned long long>(slowFrames_), maxGapMs_, static_cast<unsigned long long>(gaps_));
  out.push_back(b);
  std::vector<std::pair<std::string, Row>> rows(rows_.begin(), rows_.end());
  std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.totalUs > b.second.totalUs; });
  const double perFrame = frames_ ? 1.0 / static_cast<double>(frames_) : 0;
  for (size_t i = 0; i < rows.size() && i < maxRows; ++i) {
    const Row& r = rows[i].second;
    std::snprintf(b, sizeof b, "perf:   %-32s %8llu calls  avg %7.1f us  max %8.2f ms  %.3f ms/frame",
                  rows[i].first.c_str(), static_cast<unsigned long long>(r.calls),
                  r.calls ? r.totalUs / static_cast<double>(r.calls) : 0.0, r.maxUs / 1000.0, r.totalUs * perFrame / 1000.0);
    out.push_back(b);
  }
  if (rows.empty()) out.push_back("perf:   (no plugin callbacks yet)");
  return out;
}

void PerfStats::Reset(double nowS) {
  rows_.clear();
  thisFrame_.clear();
  frameUs_ = 0;
  frames_ = slowFrames_ = gaps_ = 0;
  maxFrameUs_ = maxGapMs_ = 0;
  lastFrameS_ = -1;
  sinceS_ = nowS;
}

}  // namespace readyup
