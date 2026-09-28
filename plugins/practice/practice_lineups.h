#pragma once

// readyup-practice lineup library (ME .savenade / .loadnade / .listnades / .deletenade /
// .importnade / .globalnades), engine-free (ctest `practice_rules`). One book per map, stored as
// <plugin data dir>/lineups/<map>.json:
//
//   {"version": 1, "lineups": {"<owner>": {"<name>": {"pos": [x, y, z], "ang": [pitch, yaw, roll],
//                                                     "kind": "smoke", "desc": "..."}}}}
//
// <owner> is the SteamID64 in decimal, or "default" for the global lineups (.globalnades), like
// ME's savednades.json. A lineup is where to stand and where to look; there is no throw in it.

#include "practice_tools.h"

#include "readyup/status_snapshot.h"

#include <map>
#include <string>
#include <vector>

namespace practice {

constexpr const char* kGlobalOwner = "default";

struct Lineup {
  std::string name;
  Vec3f pos;
  Vec3f ang;
  std::string kind;  // GrenadeKind() of the thrower's last grenade, "" when unknown
  std::string desc;
};

class LineupBook {
 public:
  // False (nothing changed) when `owner` already has a lineup called l.name (ME: use .delnade first).
  bool Add(const std::string& owner, const Lineup& l);
  bool Remove(const std::string& owner, const std::string& name);
  const Lineup* Find(const std::string& owner, const std::string& name) const;
  // `owner`'s lineups whose name contains `filter` (case-insensitive; "" = all), sorted by name.
  std::vector<const Lineup*> List(const std::string& owner, const std::string& filter) const;
  // ME .loadnade: `owner`'s lineups first, then the global ones; the nearest name in each (NearestName).
  // *fromOwner is set to the owner it came from.
  const Lineup* Load(const std::string& owner, const std::string& query, std::string* fromOwner) const;
  size_t Count() const;
  bool Empty() const { return Count() == 0; }

  readyup::status::Json ToJson() const;  // {"lineups": {...}} (json_store adds "version")
  // Replaces the book. Malformed entries are skipped (counted in *skipped).
  void FromJson(const readyup::status::Json& doc, int* skipped);

 private:
  std::map<std::string, std::map<std::string, Lineup>> byOwner_;
};

// ME StringSimilarity.FindNearestName: an exact (case-insensitive) name wins; a one-letter query
// takes the first name starting with it; otherwise the best Dice coefficient over bigrams, and
// "" when the best is below 0.25 (ME had no floor: any shared bigram matched).
std::string NearestName(const std::string& query, const std::vector<std::string>& names);
double DiceCoefficient(const std::string& a, const std::string& b);

// ME .importnade code: "<name> x y z pitch yaw roll" (7 parts; commas are ignored, so
// "x, y, z" works too). False on anything else.
bool ParseImportCode(const std::string& code, std::string* name, Vec3f* pos, Vec3f* ang);
// The code .savenade prints, which .importnade takes back.
std::string ExportCode(const Lineup& l);

// A lineup name: 1..32 of [A-Za-z0-9_-.]. ME took the first word; so do the commands.
bool ValidLineupName(const std::string& name);
// A map name that is safe as a file name ([a-z0-9_-], 1..64), else "".
std::string MapFileStem(const std::string& map);

}  // namespace practice
