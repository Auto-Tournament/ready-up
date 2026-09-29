// ru_api grenade_spawn rules (core/src/readyup/grenade_spec.h): type table and request checks.
#include "readyup/grenade_spec.h"
#include "readyup/plugin_api.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace g = readyup::grenades;

static int g_failed = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_failed;
}

int main() {
  // The table matches the public RU_GRENADE_* values and CS2's item definition indices.
  Check(g::Info(RU_GRENADE_SMOKE) && g::Info(RU_GRENADE_SMOKE)->item_def == 45, "smoke = item 45");
  Check(g::Info(RU_GRENADE_FLASH) && g::Info(RU_GRENADE_FLASH)->item_def == 43, "flash = item 43");
  Check(g::Info(RU_GRENADE_HE) && g::Info(RU_GRENADE_HE)->item_def == 44, "he = item 44");
  Check(g::Info(RU_GRENADE_MOLOTOV) && g::Info(RU_GRENADE_MOLOTOV)->item_def == 46, "molotov = item 46");
  Check(g::Info(RU_GRENADE_INCENDIARY) && g::Info(RU_GRENADE_INCENDIARY)->item_def == 48, "incendiary = item 48");
  Check(g::Info(RU_GRENADE_DECOY) && g::Info(RU_GRENADE_DECOY)->item_def == 47, "decoy = item 47");
  Check(!g::Info(0) && !g::Info(7) && !g::Info(0xFFFFFFFFu), "unknown types have no entry");
  Check(std::strcmp(g::Info(RU_GRENADE_MOLOTOV)->key, g::Info(RU_GRENADE_INCENDIARY)->key) == 0,
        "molotov and incendiary share CMolotovProjectile_Create");
  int team = 0, flt = 0;
  for (uint32_t t = g::kFirstType; t <= g::kLastType; ++t) {
    team += g::Info(t)->takes_team;
    flt += g::Info(t)->takes_float;
  }
  Check(team == 1 && g::Info(RU_GRENADE_SMOKE)->takes_team, "only smoke takes the team argument");
  Check(flt == 1 && g::Info(RU_GRENADE_HE)->takes_float, "only HE takes the float argument");

  const float ok[3] = {-1200.f, 800.f, 64.f}, zero[3] = {0, 0, 0}, vel[3] = {500.f, -300.f, 250.f};
  Check(g::Validate(RU_GRENADE_SMOKE, ok, zero, vel, zero) == nullptr, "a normal throw passes");
  Check(g::Validate(9, ok, zero, vel, zero) != nullptr, "unknown type refused");
  const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
  const float bad1[3] = {nan, 0, 0}, bad2[3] = {0, inf, 0}, far[3] = {0, 0, 40000.f}, fast[3] = {0, 0, 20000.f};
  Check(g::Validate(RU_GRENADE_HE, bad1, zero, vel, zero) != nullptr, "NaN origin refused");
  Check(g::Validate(RU_GRENADE_HE, far, zero, vel, zero) != nullptr, "off-map origin refused");
  Check(g::Validate(RU_GRENADE_HE, ok, bad2, vel, zero) != nullptr, "infinite angles refused");
  Check(g::Validate(RU_GRENADE_HE, ok, zero, fast, zero) != nullptr, "absurd velocity refused");
  Check(g::Validate(RU_GRENADE_HE, ok, zero, bad1, zero) != nullptr, "NaN velocity refused");
  Check(g::Validate(RU_GRENADE_HE, ok, zero, vel, bad2) != nullptr, "infinite angular velocity refused");
  Check(g::Validate(RU_GRENADE_HE, nullptr, zero, vel, zero) != nullptr, "missing origin refused");

  // The public struct: fixed-width fields, owner_slot last (struct_size checks rely on it).
  Check(offsetof(ru_grenade_spawn, origin) == 8 && offsetof(ru_grenade_spawn, owner_slot) == 56 &&
            sizeof(ru_grenade_spawn) == 60,
        "ru_grenade_spawn layout");

  std::printf("%s\n", g_failed ? "grenade_spec: FAILED" : "grenade_spec: all passed");
  return g_failed ? 1 : 0;
}
