#pragma once
// Map list entries and loaded map names, including Steam Workshop maps (docs/PARITY.md
// "Workshop maps"). Engine-free; unit-tested by tests/fleet_state_test.cpp.
//
// A map entry (MAT `maplist`, match.assign `maps[].name` / `workshop_id`, cmd change_map) is one of
//
//   "de_dust2"                      an installed map: `changelevel de_dust2`
//   "3084291314"                    a workshop id (all digits)
//   "workshop/3084291314"           a workshop id
//   "ws:3084291314"                 a workshop id
//   "workshop/3084291314/aim_map"   a workshop id and the map it loads
//
// A workshop entry loads with `host_workshop_map <id>`. The engine then reports the map by its
// bsp name (`Loading map "aim_map"`, CS2 1.41); other sources name it "workshop/<id>/aim_map".
// LoadedBaseName() reduces both to "aim_map", which is what map tracking, MatchState and demo
// names use. A workshop entry without a name is bound to the name the next map load reports
// after its host_workshop_map (NoteWorkshopLoad + NoteMapLoaded), so the entry is recognised
// as loaded from then on. Thread-safe.
#include <string>

namespace readyup::mapnames {

struct MapRef {
  std::string workshop_id;  // "" = not a workshop map
  std::string name;         // bsp name; "" = not known yet (workshop id only)
};

// False for an empty entry, one with characters that cannot go on a command line (only
// [A-Za-z0-9_./-], no ".."), or a malformed workshop id (1..20 digits).
bool ParseEntry(const std::string& entry, MapRef* out);
inline bool ValidEntry(const std::string& entry) { return ParseEntry(entry, nullptr); }

// The entry for a map given as name + workshop id (match.assign maps[]): the name alone without
// an id; "workshop/<id>/<name>" with one (the id wins over an id inside the name); "" if invalid.
std::string MakeEntry(const std::string& name, const std::string& workshopId);

// `host_workshop_map <id>` or `changelevel <name>`; "" for an invalid entry.
std::string LoadCommand(const std::string& entry);

// "workshop/<id>/aim_map" -> "aim_map", "maps/aim_map.vpk" -> "aim_map", "de_dust2" -> "de_dust2".
std::string LoadedBaseName(const std::string& loaded);
// The workshop id inside a loaded map name ("workshop/<id>/..."), else "".
std::string WorkshopIdOfLoaded(const std::string& loaded);

// True when `loaded` (as the engine reports it) is the map `entry` loads. Case-insensitive.
bool EntryMatchesLoaded(const std::string& entry, const std::string& loaded);

// For status / chat: the map name, the bound name of a workshop id, else "workshop/<id>".
std::string DisplayName(const std::string& entry);

// Binding of workshop ids to map names: a host_workshop_map for `workshopId` was issued ...
void NoteWorkshopLoad(const std::string& workshopId);
// ... and a map finished loading (any map change): binds the pending id (and an id inside the
// loaded name) to its base name.
void NoteMapLoaded(const std::string& loaded);
std::string BoundName(const std::string& workshopId);  // "" when unknown
void ResetBindings();                                  // tests

}  // namespace readyup::mapnames
