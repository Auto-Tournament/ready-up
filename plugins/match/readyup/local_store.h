#pragma once

// readyup-match's local persistence (docs/FLEET.md D13: no Postgres). Three small JSON files in
// the plugin data dir (csgo/readyup/plugins/match/), written through libs/readyup/json_store.h
// (atomic replace, versioned, corrupt files moved aside):
//
//   state.json         {"version":1,"settings":{key: value}}  settings that survive a reboot
//                      (ru_webhook_url, ru_match_token, ...) and the crash-recovery match state
//                      (persisted_match_state.h); replaces the readyup_settings table
//   admins.json        {"version":1,"admins":[{"steamid64":"7656...","name":"..."}]}  standalone
//                      admins (`ru admins add|remove`, or edited by hand: picked up within 30 s);
//                      replaces the readyup_admins table
//   fleet-admins.json  {"version":1,"rev":N,"admins":[...]}  the platform's list (admins.set, D5),
//                      cached so a server that boots offline still knows its admins
//
// Reads come from memory and never block. Writes update memory at once and are saved by one
// writer thread (coalesced; unload saves what is pending). Any thread.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace readyup::local_store {

struct Admin {
  uint64_t steamid64 = 0;
  std::string name;
};

// Plugin load, before anything reads (loads the three files; logs and moves aside corrupt ones).
void Init(const std::string& dataDir);
std::string Path(const char* file);  // <dataDir>/<file>

// ---- settings (key/value) ----
void SetSetting(const std::string& key, std::optional<std::string> value);  // nullopt clears
std::optional<std::string> GetSetting(const std::string& key);             // empty reads as nullopt

// ---- standalone admins (admins.json) ----
std::vector<Admin> LocalAdmins();
bool AddLocalAdmin(uint64_t steamid64, const std::string& name);  // false: already an admin
bool RemoveLocalAdmin(uint64_t steamid64);                        // false: was not one
bool IsLocalAdmin(uint64_t steamid64);
// Re-reads admins.json when it changed on disk (hand edits). Worker thread.
void ReloadAdminsIfChanged();

// ---- fleet admins (admins.set) ----
// Fleet mode: the fleet link is configured (fleet_iface.h connection_state != STANDALONE). Then
// only the platform's list counts and `ru admins add|remove` are refused (D5).
void SetFleetMode(bool on);
bool FleetMode();
// false when `rev` is older than the stored list (a replay), which is kept.
bool SetFleetAdmins(int64_t rev, std::vector<Admin> admins);
std::vector<Admin> FleetAdmins(int64_t* rev = nullptr);
bool IsFleetAdmin(uint64_t steamid64);

// `ru selftest` line.
std::string Summary();

}  // namespace readyup::local_store
