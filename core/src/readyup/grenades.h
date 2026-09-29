#pragma once

// Grenade projectiles for ru_api grenade_spawn / grenade_spawn_available (API 1.12).
//
// The five Create functions come from the practice gamedata fragment
// (gamedata/engine-surface.practice.json), which ships only with the practice plugin. Each one
// must match exactly once and pass its anchors (its projectile classname, and a call from CS2's own
// point_script SpawnGrenadeProjectile binding), otherwise that grenade type is unavailable and
// Spawn() returns nullptr without calling anything. Game thread only.

#include <cstdint>
#include <string>

namespace readyup::grenades {

// True if the Create function behind `type` (RU_GRENADE_*) verified on this build.
bool Available(uint32_t type);

struct SpawnRequest {
  uint32_t type = 0;
  const float* origin = nullptr;        // [3]
  const float* angles = nullptr;        // [3]
  const float* velocity = nullptr;      // [3]
  const float* ang_velocity = nullptr;  // [3]
  void* owner_pawn = nullptr;           // thrower (a live CCSPlayerPawn) or nullptr
  int team = 0;                         // the thrower's team (smoke's extra argument)
};

// The new projectile entity, or nullptr with *why set (refused before the engine call, or the
// function is unavailable).
void* Spawn(const SpawnRequest& req, std::string* why);

}  // namespace readyup::grenades
