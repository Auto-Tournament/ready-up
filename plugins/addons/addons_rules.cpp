#include "addons_rules.h"

#include <algorithm>
#include <cctype>

namespace addons {

std::vector<uint64_t> ParseIds(const std::string& text, int* bad) {
  std::vector<uint64_t> out;
  if (bad) *bad = 0;
  std::string tok;
  auto flush = [&] {
    if (tok.empty()) return;
    const bool digits = tok.size() <= 20 && std::all_of(tok.begin(), tok.end(), [](unsigned char c) { return std::isdigit(c); });
    uint64_t v = 0;
    if (digits) {
      try {
        v = std::stoull(tok);
      } catch (...) {
        v = 0;
      }
    }
    if (v == 0) {
      if (bad) ++*bad;
    } else if (std::find(out.begin(), out.end(), v) == out.end()) {
      out.push_back(v);
    }
    tok.clear();
  };
  for (char c : text) {
    if (c == ',' || c == ' ' || c == '\t' || c == ';') flush();
    else tok.push_back(c);
  }
  flush();
  return out;
}

std::string ItemStateText(uint32_t s) {
  if (s & kItemLegacy) return "legacy (not a Source 2 addon)";
  if (s & kItemDownloading) return "downloading";
  if (s & kItemDownloadPending) return "download queued";
  if (s & kItemInstalled) return (s & kItemNeedsUpdate) ? "installed, update available" : "installed";
  return "not installed";
}

Action NextAction(uint32_t s, bool mounted, bool downloadAsked) {
  if (s & kItemLegacy) return Action::kRefuse;
  if (s & (kItemDownloading | kItemDownloadPending)) return Action::kNone;
  if (!(s & kItemInstalled)) return downloadAsked ? Action::kNone : Action::kDownload;
  if (mounted) return Action::kNone;
  return Action::kMount;
}

}  // namespace addons
