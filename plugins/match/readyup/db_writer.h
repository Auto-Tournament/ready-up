#pragma once

// Ordered background writes of readyup-match's key/value settings (pg::SetSetting / ClearSetting).
//
// One worker thread drains a FIFO, so writes land in the order they were made (the core version
// started one detached thread per write). Unload drains what is queued (each write is bounded
// by libpq's connect timeout) and joins the thread.

#include <optional>
#include <string>

namespace readyup::db_writer {

// Any thread. value = nullopt clears the key. No-op without Postgres support / config.
void SetSettingAsync(std::string key, std::optional<std::string> value);

}  // namespace readyup::db_writer
