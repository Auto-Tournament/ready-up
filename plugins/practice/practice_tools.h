#pragma once

// readyup-practice pure logic for the ME practice extras (ctest `practice_rules`): grenade throw
// history (.last / .back N / .lastindex / .delay), closest / farthest spawn (.bestspawn /
// .worstspawn), the cvar toggles (.solid / .impacts / .traj) and the .timer text. Engine-free.

#include <cstddef>
#include <string>
#include <vector>

namespace practice {

struct Vec3f {
  float x = 0, y = 0, z = 0;
};

// ME's grenade kinds: "smoke" "flash" "hegrenade" "molotov" "decoy" ("" when not a grenade).
// Takes a grenade_thrown `weapon` ("smokegrenade", "flashbang", "incgrenade", ...), with or
// without the "weapon_" prefix.
std::string GrenadeKind(const std::string& weapon);

// The kind a typed ME rethrow command asks for (.rethrowsmoke / .throwsmoke -> "smoke", ...,
// .rethrownade / .thrownade / .rethrowgrenade / .throwgrenade -> "hegrenade"), else "".
std::string TypedRethrowKind(const std::string& cmd);

struct Throw {
  Vec3f pos;  // where the thrower stood
  Vec3f ang;  // their eye angles
  std::string kind;
  float delay = 0;  // .delay: seconds before .rethrow runs it
};

// One player's throws, oldest first; ME numbers them 1..Count().
class ThrowHistory {
 public:
  explicit ThrowHistory(size_t cap = 100) : cap_(cap) {}
  void Add(const Throw& t);
  size_t Count() const { return items_.size(); }
  // 1-based; nullptr when out of range.
  const Throw* At(int n) const;
  Throw* Last() { return items_.empty() ? nullptr : &items_.back(); }
  const Throw* Last() const { return items_.empty() ? nullptr : &items_.back(); }
  const Throw* LastOfKind(const std::string& kind) const;

 private:
  size_t cap_;
  std::vector<Throw> items_;
};

// "3" -> 3; nothing else (0, negatives, "2x", "") parses. ME: a valid non-negative number >= 1.
bool ParsePositiveInt(const std::string& s, int* out);
// ME .delay: a float > 0 (capped at 60 s here so a typo cannot park a rethrow for hours).
bool ParseDelaySeconds(const std::string& s, float* out);

// Index of the point closest to (farthest from) `from`, -1 when empty.
int ClosestIndex(const std::vector<Vec3f>& pts, const Vec3f& from);
int FarthestIndex(const std::vector<Vec3f>& pts, const Vec3f& from);

// ME .solid: mp_solid_teammates 0/1 -> 2, else -> 1.
int NextSolidValue(int current);

// ME .timer result, "12.34".
std::string FormatTimerSeconds(double secs);

}  // namespace practice
