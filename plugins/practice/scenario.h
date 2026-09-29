#pragma once

// readyup-practice scenarios ("pro round replay"): the engine-free part. A scenario is one recorded
// round (tools/scenario/ru_scenario.py turns a demo round into one; schema in docs/SCENARIOS.md):
// every player's path, their health / armor / money / items over time, grenades, deaths and the bomb.
// This file parses and checks it, interpolates positions, picks players and schedules the timeline.
// The engine side (bots, teleports, grenades) is practice_scenarios.cpp. ctest `practice_scenario`.

#include <cstdint>
#include <string>
#include <vector>

namespace practice::scenario {

constexpr const char* kFormat = "readyup.scenario";
constexpr int kVersion = 1;
constexpr int kMaxPlayers = 10;

struct V3 {
  float x = 0, y = 0, z = 0;
};

struct Pose {
  V3 pos;
  float yaw = 0, pitch = 0;
};

// Health, armor, money and items from tick t (after freeze end) on.
struct State {
  int t = 0;
  int hp = 100;
  int armor = 0;
  bool helmet = false;
  bool defuser = false;
  int money = 0;
  std::vector<std::string> items;  // weapon_* classnames (duplicates = several grenades of a kind)
};

struct Player {
  std::string name;
  std::string steamid;
  int team = 0;         // 2 = T, 3 = CT
  int aliveUntil = -1;  // tick (after freeze end) the recorded player died, -1 = survived
  std::vector<float> track;  // x y z yaw pitch per sample (every sample_ticks from t = 0)
  std::vector<State> states;  // first at t = 0, ascending

  int Samples() const { return static_cast<int>(track.size() / 5); }
  // Alive in the recording at tick t.
  bool AliveAt(int t) const { return aliveUntil < 0 || t < aliveUntil; }
  // Position and view at tick t (linear between samples; yaw takes the short way round). False
  // past the end of the track (dead, or the recording ended).
  bool PoseAt(double t, int sampleTicks, Pose* out) const;
  const State& StateAt(int t) const;
};

enum class GrenadeType { kSmoke, kFlash, kHe, kMolotov, kIncendiary, kDecoy };

struct Grenade {
  int t = 0;       // spawn tick (after freeze end)
  int player = 0;  // thrower (index into players)
  GrenadeType type = GrenadeType::kSmoke;
  V3 pos, vel;     // projectile spawn point and velocity (units/s)
  V3 land;         // where it detonated / landed
  int landT = 0;
};

struct Death {
  int t = 0;
  int player = 0;
  int killer = -1;  // -1: not a scenario player (world, bomb, disconnected)
  std::string weapon;
};

struct BombEvent {
  int t = 0;
  std::string event;  // planted, defused, exploded
  int player = -1;
  bool hasPos = false;
  V3 pos;
};

struct Source {
  std::string event, match, teamT, teamCT, url, demo, note;
};

struct Scenario {
  std::string id, title, map;
  int tickrate = 64;
  int sampleTicks = 4;
  int round = 0;
  int freezeEndTick = 0;
  int length = 0;  // ticks from freeze end to the end of the recording
  int defaultStart = 0;
  std::string winner, reason;
  Source source;
  std::vector<Player> players;
  std::vector<Grenade> grenades;  // by t
  std::vector<Death> deaths;      // by t
  std::vector<BombEvent> bomb;    // by t
};

// Parses and validates (format, version, ranges, indices). false + *err on anything wrong.
bool Parse(const std::string& json, Scenario* out, std::string* err);
bool ParseFile(const std::string& path, Scenario* out, std::string* err);

const char* GrenadeName(GrenadeType t);  // "smoke", "flash", ...
int GrenadeDefIndex(GrenadeType t);      // CS2 item definition index (43 flash .. 48 incgrenade)
bool IsValidId(const std::string& id);   // [a-z0-9_-]{1,64}

// Start of the replay: "25" / "25s" / "1:05" = seconds after freeze end; "t12345" / "12345t" = a demo
// tick. -> ticks after freeze end, clamped to [0, length - 1]. false + *err when unparsable.
bool ParseStart(const std::string& text, const Scenario& sc, int* out, std::string* err);

// Player by "3" (1-based), SteamID64, or name (case-insensitive: exact, then prefix, then substring;
// ambiguous = error). -1 + *err when none matches.
int FindPlayer(const Scenario& sc, const std::string& query, std::string* err);
// The first player of `team` alive at startT, else the first player alive at startT, else -1.
int DefaultPlayer(const Scenario& sc, int team, int startT);

// Bots per team for a replay: players alive at startT, minus `human`.
void BotCounts(const Scenario& sc, int human, int startT, int* t, int* ct);

// Timeline entries in (prevT, nowT], in time order.
struct Due {
  enum Kind { kGrenade, kDeath, kBomb } kind;
  int index;
};
std::vector<Due> DueBetween(const Scenario& sc, int prevT, int nowT);

// "de_nuke r6 · Team Vitality (T) vs Team Spirit (CT) · 109 s"
std::string Summary(const Scenario& sc);
// "1 apEX (T), 2 flameZ (T), ..."
std::string PlayerList(const Scenario& sc);
// "Team Vitality vs Team Spirit, map 2 · ESL ... · https://..."
std::string Attribution(const Scenario& sc);
// "1:05" from ticks.
std::string Clock(int ticks, int tickrate);

}  // namespace practice::scenario
