#include "readyup/grenades.h"

#include "readyup/engine_surface.h"
#include "readyup/entity.h"
#include "readyup/grenade_spec.h"
#include "readyup/logging.h"

#include <cstring>

namespace readyup::grenades {
namespace {

// The Create functions, as CS2's point_script SpawnGrenadeProjectile calls them (checked against
// that call site in libserver.so 1.41.8.5; the verified signatures cover the prologue's argument
// moves): rdi origin, rsi angles, rdx velocity, rcx angular velocity, r8 thrower pawn (may be
// null: then no thrower and team 0), r9d item definition index. Smoke takes the team as a 7th
// (stack) argument; for HE the script also loads 3.0f into xmm0, which we repeat.
using CreateFn = void* (*)(const float*, const float*, const float*, const float*, void*, int);
using CreateSmokeFn = void* (*)(const float*, const float*, const float*, const float*, void*, int, int);
using CreateHeFn = void* (*)(const float*, const float*, const float*, const float*, void*, int, float);

// Vector / QAngle: three packed floats, only read by the engine. Padded and aligned like
// entity::SetAbsOrigin's.
struct Vec {
  alignas(16) float v[4];
  explicit Vec(const float* in) : v{in[0], in[1], in[2], 0.f} {}
};

}  // namespace

bool Available(uint32_t type) {
  const TypeInfo* t = Info(type);
  return t && EngineFunction(t->key) != nullptr;
}

void* Spawn(const SpawnRequest& req, std::string* why) {
  auto fail = [&](const char* w) -> void* {
    if (why) *why = w;
    return nullptr;
  };
  if (const char* bad = Validate(req.type, req.origin, req.angles, req.velocity, req.ang_velocity)) return fail(bad);
  const TypeInfo* t = Info(req.type);
  void* fn = EngineFunction(t->key);
  if (!fn) return fail("not available on this CS2 build (its gamedata did not verify, or the practice gamedata is not installed)");
  if (!entity::EntitySystemReady()) return fail("no map loaded yet");

  const Vec origin(req.origin), angles(req.angles), velocity(req.velocity), angVel(req.ang_velocity);
  void* ent = nullptr;
  if (t->takes_team) {
    ent = reinterpret_cast<CreateSmokeFn>(fn)(origin.v, angles.v, velocity.v, angVel.v, req.owner_pawn, t->item_def,
                                              req.team);
  } else if (t->takes_float) {
    ent = reinterpret_cast<CreateHeFn>(fn)(origin.v, angles.v, velocity.v, angVel.v, req.owner_pawn, t->item_def, 3.0f);
  } else {
    ent = reinterpret_cast<CreateFn>(fn)(origin.v, angles.v, velocity.v, angVel.v, req.owner_pawn, t->item_def);
  }
  if (!ent) return fail("the engine did not create the projectile");
  // Sanity: the entity must be what this function is documented to create.
  const char* cls = entity::EntityDesignerName(ent);
  if (cls && std::strcmp(cls, t->classname) != 0) {
    Print("grenades: %s Create returned a '%s', expected '%s'\n", t->name, cls, t->classname);
  }
  return ent;
}

}  // namespace readyup::grenades
