#include "practice_tools.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace practice {
namespace {
std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
double Dist2(const Vec3f& a, const Vec3f& b) {
  const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
  return dx * dx + dy * dy + dz * dz;
}
}  // namespace

std::string GrenadeKind(const std::string& weapon) {
  std::string w = Lower(weapon);
  if (w.rfind("weapon_", 0) == 0) w = w.substr(7);
  if (w == "smokegrenade") return "smoke";
  if (w == "flashbang") return "flash";
  if (w == "hegrenade") return "hegrenade";
  if (w == "molotov" || w == "incgrenade") return "molotov";
  if (w == "decoy") return "decoy";
  return {};
}

std::string ProjectileKind(const std::string& classname) {
  const std::string c = Lower(classname);
  if (c == "smokegrenade_projectile") return "smoke";
  if (c == "flashbang_projectile") return "flash";
  if (c == "hegrenade_projectile") return "hegrenade";
  if (c == "molotov_projectile") return "molotov";
  if (c == "decoy_projectile") return "decoy";
  return {};
}

std::string TypedRethrowKind(const std::string& cmd) {
  std::string c = Lower(cmd);
  if (c.rfind(".rethrow", 0) == 0) c = c.substr(8);
  else if (c.rfind(".throw", 0) == 0) c = c.substr(6);
  else return {};
  if (c == "smoke") return "smoke";
  if (c == "flash") return "flash";
  if (c == "nade" || c == "grenade") return "hegrenade";
  if (c == "molotov") return "molotov";
  if (c == "decoy") return "decoy";
  return {};
}

void ThrowHistory::Add(const Throw& t) {
  items_.push_back(t);
  if (cap_ && items_.size() > cap_) items_.erase(items_.begin());
}

const Throw* ThrowHistory::At(int n) const {
  if (n < 1 || static_cast<size_t>(n) > items_.size()) return nullptr;
  return &items_[static_cast<size_t>(n - 1)];
}

const Throw* ThrowHistory::LastOfKind(const std::string& kind) const {
  for (auto it = items_.rbegin(); it != items_.rend(); ++it) {
    if (it->kind == kind) return &*it;
  }
  return nullptr;
}

Throw* ThrowHistory::AwaitingLaunch(const std::string& kind, double since) {
  Throw* oldest = nullptr;
  for (auto it = items_.rbegin(); it != items_.rend(); ++it) {
    if (it->at < since) break;
    if (it->kind == kind && !it->launch.set) oldest = &*it;
  }
  return oldest;
}

bool ParsePositiveInt(const std::string& s, int* out) {
  if (s.empty() || s.size() > 9) return false;
  for (char c : s) {
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
  }
  const int v = std::atoi(s.c_str());
  if (v < 1) return false;
  *out = v;
  return true;
}

bool ParseDelaySeconds(const std::string& s, float* out) {
  if (s.empty()) return false;
  char* end = nullptr;
  const float v = std::strtof(s.c_str(), &end);
  if (!end || *end != '\0' || !std::isfinite(v) || v <= 0.f || v > 60.f) return false;
  *out = v;
  return true;
}

int ClosestIndex(const std::vector<Vec3f>& pts, const Vec3f& from) {
  int best = -1;
  double bestD = std::numeric_limits<double>::max();
  for (size_t i = 0; i < pts.size(); ++i) {
    const double d = Dist2(pts[i], from);
    if (d < bestD) {
      bestD = d;
      best = static_cast<int>(i);
    }
  }
  return best;
}

int FarthestIndex(const std::vector<Vec3f>& pts, const Vec3f& from) {
  int best = -1;
  double bestD = -1;
  for (size_t i = 0; i < pts.size(); ++i) {
    const double d = Dist2(pts[i], from);
    if (d > bestD) {
      bestD = d;
      best = static_cast<int>(i);
    }
  }
  return best;
}

int NextSolidValue(int current) { return (current == 0 || current == 1) ? 2 : 1; }

std::string FormatTimerSeconds(double secs) {
  if (!std::isfinite(secs) || secs < 0) secs = 0;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.2f", secs);
  return buf;
}

}  // namespace practice
