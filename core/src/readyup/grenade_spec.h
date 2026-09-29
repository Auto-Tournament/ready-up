#pragma once

// Pure rules of ru_api grenade_spawn (API 1.12), kept free of engine code so they are unit
// tested (tests/grenade_spec_test.cpp): which engine-surface function spawns which grenade type,
// with which item definition index, and which spawn requests are refused before any engine call.

#include <cmath>
#include <cstdint>

namespace readyup::grenades {

// Values of RU_GRENADE_* in plugin_api.h.
enum Type : uint32_t {
  kSmoke = 1,
  kFlash = 2,
  kHe = 3,
  kMolotov = 4,
  kIncendiary = 5,
  kDecoy = 6,
};

struct TypeInfo {
  uint32_t type;
  const char* name;       // "smoke", ... (logs, `ru selftest`)
  const char* key;        // gamedata/engine-surface.practice.json "functions" key
  int item_def;           // CS2 item definition index passed to Create
  const char* classname;  // designer name of the entity Create returns
  bool takes_team;        // smoke: 7th argument (team of the thrower)
  bool takes_float;       // HE: the script call passes 3.0f in xmm0; so do we
};

// Nullptr for an unknown type.
inline const TypeInfo* Info(uint32_t type) {
  static const TypeInfo k[] = {
      {kSmoke, "smoke", "CSmokeGrenadeProjectile_Create", 45, "smokegrenade_projectile", true, false},
      {kFlash, "flash", "CFlashbangProjectile_Create", 43, "flashbang_projectile", false, false},
      {kHe, "he", "CHEGrenadeProjectile_Create", 44, "hegrenade_projectile", false, true},
      {kMolotov, "molotov", "CMolotovProjectile_Create", 46, "molotov_projectile", false, false},
      {kIncendiary, "incendiary", "CMolotovProjectile_Create", 48, "molotov_projectile", false, false},
      {kDecoy, "decoy", "CDecoyProjectile_Create", 47, "decoy_projectile", false, false},
  };
  for (const auto& t : k) {
    if (t.type == type) return &t;
  }
  return nullptr;
}

constexpr uint32_t kFirstType = kSmoke;
constexpr uint32_t kLastType = kDecoy;

// World coordinates beyond this are off any map (and NaN / inf never pass).
constexpr float kMaxCoord = 32768.f;
// Faster than any throw (sv_maxvelocity is 3500); a bigger vector is a caller bug.
constexpr float kMaxSpeed = 10000.f;

inline bool Finite3(const float v[3], float limit) {
  for (int i = 0; i < 3; ++i) {
    if (!std::isfinite(v[i]) || v[i] <= -limit || v[i] >= limit) return false;
  }
  return true;
}

// Why a request is refused before the engine is called, or nullptr if it may go ahead.
inline const char* Validate(uint32_t type, const float origin[3], const float angles[3], const float velocity[3],
                            const float ang_velocity[3]) {
  if (!Info(type)) return "unknown grenade type";
  if (!origin || !Finite3(origin, kMaxCoord)) return "origin is not a finite map position";
  if (!angles || !Finite3(angles, 360.f * 16)) return "angles are not finite";
  if (!velocity || !Finite3(velocity, kMaxSpeed)) return "velocity is not finite or too large";
  if (!ang_velocity || !Finite3(ang_velocity, 1.0e5f)) return "angular velocity is not finite";
  return nullptr;
}

}  // namespace readyup::grenades
