#pragma once
// Pure logic of the addons plugin (no engine): the workshop id list and Steam item states.
#include <cstdint>
#include <string>
#include <vector>

namespace addons {

// "3808995594, 123" -> {3808995594, 123}: comma / space separated, first occurrence kept, entries
// that are not plain positive integers skipped (*bad counts them).
std::vector<uint64_t> ParseIds(const std::string& text, int* bad);

// ISteamUGC::GetItemState bits (EItemState).
enum ItemState : uint32_t {
  kItemSubscribed = 1,
  kItemLegacy = 2,
  kItemInstalled = 4,
  kItemNeedsUpdate = 8,
  kItemDownloading = 16,
  kItemDownloadPending = 32,
};
// "installed", "installed, needs update", "downloading", "not installed", ...
std::string ItemStateText(uint32_t state);

// What to do with an item this tick.
enum class Action { kNone, kDownload, kMount, kRefuse };
// `downloadAsked`: a download was already requested (and Steam has not reported it installed yet).
Action NextAction(uint32_t state, bool mounted, bool downloadAsked);

}  // namespace addons
