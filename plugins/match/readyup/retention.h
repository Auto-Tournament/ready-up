#pragma once

// File retention sweep: deletes old round backups / resume files and old GOTV demos (which files:
// retention_rules.h; readyup.cfg `backup_keep_hours`, `demo_keep_hours`).
//
// Runs at plugin load and at every map start, on a worker thread (directory listings of the
// backup and demo dirs). The game thread only collects what must be kept: the loaded match, the
// match a restarted server recovers (state.json) and the demo being recorded. fleet.so's own
// deletion of streamed demos ([fleet] demo_keep_hours after the platform stored them) is separate;
// while fleet.so has a streamed demo the platform has not confirmed (the local file is the only
// copy), no demo is deleted.
//
// Log line (only when something was deleted or failed):
//   `retention: deleted 3 round backups (72 h) and 1 demo (24 h)`.

namespace readyup::retention {

// Game thread.
void SweepAsync();

}  // namespace readyup::retention
