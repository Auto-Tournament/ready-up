#pragma once

// File retention: the pure half (no engine calls; ctest `match_retention`). The sweep itself is
// retention.h.
//
// Ready Up leaves files behind that nothing else deletes:
//   - CS2 round backups, readyup_backup_<matchid>_map<N>_round<NN>.txt (mp_backup_round_file), and
//     the resume copies fleet_bridge.cpp writes, readyup_resume_<matchid>_map<N>_round<NN>.txt, in
//     backup_files::BackupDirs(),
//   - GOTV demos (*.dem) in the demo dir (<write path>/<ru_demo_path>).
// readyup.cfg `backup_keep_hours` (default 72) and `demo_keep_hours` (default 24) say how long a
// file is kept after it was last written; 0 = keep forever. Files of a protected match (the loaded
// one, and the one a restarted server recovers) are never selected, whatever their age, nor are
// the demos named in `keepNames` (the recording in progress).

#include <cstdint>
#include <string>
#include <vector>

namespace readyup::retention {

struct FileEntry {
  std::string name;     // basename
  long long mtime = 0;  // epoch seconds
};

// readyup_backup_<id>_map<N>_round<NN>.txt or readyup_resume_<id>_map<N>_round<NN>.txt: sets *matchid.
// False for anything else (CS2's own backup_round*.txt, temp files, other names).
bool ParseRetainedBackupName(const std::string& name, uint64_t* matchid);

// A demo name that carries `matchid` as a token: "<id>_..." or "..._<id>_..." / "..._<id>.dem"
// (ru_demo_name_format's {MATCH_ID}, as demo::PickDemoForMatch looks for it). False for 0.
bool DemoNameHasMatchId(const std::string& name, uint64_t matchid);

// True when a file last written at `mtime` is older than `keepHours` at `now` (keepHours <= 0:
// never; a future mtime: never).
bool Expired(long long now, long long mtime, int keepHours);

// Backup / resume files to delete: expired and not of a protected match.
std::vector<std::string> BackupsToDelete(const std::vector<FileEntry>& files, long long now, int keepHours,
                                         const std::vector<uint64_t>& protectedMatchids);

// .dem files to delete: expired, not in keepNames, not of a protected match.
std::vector<std::string> DemosToDelete(const std::vector<FileEntry>& files, long long now, int keepHours,
                                       const std::vector<uint64_t>& protectedMatchids,
                                       const std::vector<std::string>& keepNames);

}  // namespace readyup::retention
