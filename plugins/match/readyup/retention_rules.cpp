#include "readyup/retention_rules.h"

#include <algorithm>
#include <cctype>

namespace readyup::retention {
namespace {

bool StartsWith(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }

bool EndsWith(const std::string& s, const std::string& p) {
  return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

// Reads 1+ digits at *pos (at most 20). False when there are none.
bool Digits(const std::string& s, size_t* pos, uint64_t* out) {
  size_t i = *pos;
  uint64_t v = 0;
  while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])) && i - *pos < 20) {
    v = v * 10 + static_cast<uint64_t>(s[i] - '0');
    ++i;
  }
  if (i == *pos) return false;
  *pos = i;
  if (out) *out = v;
  return true;
}

bool Literal(const std::string& s, size_t* pos, const char* lit) {
  const std::string l(lit);
  if (s.compare(*pos, l.size(), l) != 0) return false;
  *pos += l.size();
  return true;
}

bool IsProtected(uint64_t id, const std::vector<uint64_t>& ids) {
  return id != 0 && std::find(ids.begin(), ids.end(), id) != ids.end();
}

}  // namespace

bool ParseRetainedBackupName(const std::string& name, uint64_t* matchid) {
  size_t pos = 0;
  if (StartsWith(name, "readyup_backup_")) pos = 15;
  else if (StartsWith(name, "readyup_resume_")) pos = 15;
  else return false;
  uint64_t id = 0;
  if (!Digits(name, &pos, &id)) return false;
  if (!Literal(name, &pos, "_map") || !Digits(name, &pos, nullptr)) return false;
  if (!Literal(name, &pos, "_round") || !Digits(name, &pos, nullptr)) return false;
  if (!Literal(name, &pos, ".txt") || pos != name.size()) return false;
  if (matchid) *matchid = id;
  return true;
}

bool DemoNameHasMatchId(const std::string& name, uint64_t matchid) {
  if (matchid == 0) return false;
  const std::string id = std::to_string(static_cast<unsigned long long>(matchid));
  if (StartsWith(name, id + "_") || StartsWith(name, id + ".")) return true;
  if (name.find("_" + id + "_") != std::string::npos) return true;
  return EndsWith(name, "_" + id + ".dem");
}

bool Expired(long long now, long long mtime, int keepHours) {
  if (keepHours <= 0 || mtime > now) return false;
  return now - mtime >= static_cast<long long>(keepHours) * 3600;
}

std::vector<std::string> BackupsToDelete(const std::vector<FileEntry>& files, long long now, int keepHours,
                                         const std::vector<uint64_t>& protectedMatchids) {
  std::vector<std::string> out;
  if (keepHours <= 0) return out;
  for (const auto& f : files) {
    uint64_t id = 0;
    if (!ParseRetainedBackupName(f.name, &id)) continue;
    if (IsProtected(id, protectedMatchids)) continue;
    if (Expired(now, f.mtime, keepHours)) out.push_back(f.name);
  }
  return out;
}

std::vector<std::string> DemosToDelete(const std::vector<FileEntry>& files, long long now, int keepHours,
                                       const std::vector<uint64_t>& protectedMatchids,
                                       const std::vector<std::string>& keepNames) {
  std::vector<std::string> out;
  if (keepHours <= 0) return out;
  for (const auto& f : files) {
    if (f.name.size() <= 4 || !EndsWith(f.name, ".dem")) continue;
    if (std::find(keepNames.begin(), keepNames.end(), f.name) != keepNames.end()) continue;
    bool mine = false;
    for (uint64_t id : protectedMatchids) mine = mine || DemoNameHasMatchId(f.name, id);
    if (mine) continue;
    if (Expired(now, f.mtime, keepHours)) out.push_back(f.name);
  }
  return out;
}

}  // namespace readyup::retention
