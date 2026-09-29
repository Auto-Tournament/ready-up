#include "scenario.h"

#include "readyup/minijson.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace practice::scenario {
namespace {

using readyup::minijson::Value;
using VT = Value::Type;

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

const Value* Get(const Value& o, const char* k) { return o.get(k); }
std::string Str(const Value& o, const char* k) {
  const Value* v = Get(o, k);
  return v && v->type == VT::String ? v->str : std::string();
}
bool Num(const Value& o, const char* k, double* out) {
  const Value* v = Get(o, k);
  if (!v || v->type != VT::Number || !std::isfinite(v->num)) return false;
  *out = v->num;
  return true;
}
int Int(const Value& o, const char* k, int def) {
  double d = 0;
  return Num(o, k, &d) && std::fabs(d) < 2e9 ? static_cast<int>(d) : def;
}
bool Bool(const Value& o, const char* k) {
  const Value* v = Get(o, k);
  return v && ((v->type == VT::Bool && v->b) || (v->type == VT::Number && v->num != 0));
}
const std::vector<Value>* Arr(const Value& o, const char* k) {
  const Value* v = Get(o, k);
  return v && v->type == VT::Array ? &v->arr : nullptr;
}
bool Vec(const Value& o, const char* k, V3* out) {
  const auto* a = Arr(o, k);
  if (!a || a->size() != 3) return false;
  float f[3];
  for (int i = 0; i < 3; ++i) {
    const Value& v = (*a)[static_cast<size_t>(i)];
    if (v.type != VT::Number || !std::isfinite(v.num) || std::fabs(v.num) >= 65536) return false;
    f[i] = static_cast<float>(v.num);
  }
  *out = {f[0], f[1], f[2]};
  return true;
}

bool Fail(std::string* err, const std::string& m) {
  if (err) *err = m;
  return false;
}

bool ParseType(const std::string& s, GrenadeType* t) {
  static const struct {
    const char* n;
    GrenadeType t;
  } k[] = {{"smoke", GrenadeType::kSmoke},     {"flash", GrenadeType::kFlash},
           {"hegrenade", GrenadeType::kHe},     {"molotov", GrenadeType::kMolotov},
           {"incgrenade", GrenadeType::kIncendiary}, {"decoy", GrenadeType::kDecoy}};
  for (const auto& e : k) {
    if (s == e.n) {
      *t = e.t;
      return true;
    }
  }
  return false;
}

float LerpAngle(float a, float b, float w) {
  float d = std::fmod(b - a, 360.f);
  if (d > 180.f) d -= 360.f;
  if (d < -180.f) d += 360.f;
  return a + d * w;
}

}  // namespace

bool Player::PoseAt(double t, int sampleTicks, Pose* out) const {
  const int n = Samples();
  if (n == 0 || t < 0 || sampleTicks <= 0) return false;
  if (!AliveAt(static_cast<int>(t))) return false;
  const double f = t / sampleTicks;
  if (f > n - 1) return false;
  const int k = std::min(static_cast<int>(f), n - 1);
  const int k2 = std::min(k + 1, n - 1);
  const float w = static_cast<float>(f - k);
  const float* a = &track[static_cast<size_t>(k) * 5];
  const float* b = &track[static_cast<size_t>(k2) * 5];
  out->pos = {a[0] + (b[0] - a[0]) * w, a[1] + (b[1] - a[1]) * w, a[2] + (b[2] - a[2]) * w};
  out->yaw = LerpAngle(a[3], b[3], w);
  out->pitch = a[4] + (b[4] - a[4]) * w;
  return true;
}

const State& Player::StateAt(int t) const {
  size_t i = 0;
  while (i + 1 < states.size() && states[i + 1].t <= t) ++i;
  return states[i];
}

bool IsValidId(const std::string& id) {
  if (id.empty() || id.size() > 64) return false;
  for (char c : id) {
    if (!(std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '_' ||
          c == '-')) {
      return false;
    }
  }
  return true;
}

bool Parse(const std::string& json, Scenario* out, std::string* err) {
  readyup::minijson::ParseError pe;
  auto root = readyup::minijson::Parse(json, &pe);
  if (!root) return Fail(err, "not JSON (" + pe.msg + " at byte " + std::to_string(pe.offset) + ")");
  const Value& r = *root;
  if (r.type != VT::Object) return Fail(err, "not a JSON object");
  if (Str(r, "format") != kFormat) return Fail(err, std::string("format is not \"") + kFormat + "\"");
  if (Int(r, "version", -1) != kVersion) {
    return Fail(err, "version " + std::to_string(Int(r, "version", -1)) + " (this build reads " +
                         std::to_string(kVersion) + ")");
  }
  Scenario sc;
  sc.id = Str(r, "id");
  if (!IsValidId(sc.id)) return Fail(err, "id must be 1-64 chars of [a-z0-9_-]");
  sc.title = Str(r, "title");
  sc.map = Str(r, "map");
  if (sc.map.empty()) return Fail(err, "map missing");
  sc.tickrate = Int(r, "tickrate", 0);
  if (sc.tickrate < 16 || sc.tickrate > 256) return Fail(err, "tickrate must be 16..256");
  sc.sampleTicks = Int(r, "sample_ticks", 0);
  if (sc.sampleTicks < 1 || sc.sampleTicks > 64) return Fail(err, "sample_ticks must be 1..64");
  sc.round = Int(r, "round", 0);
  sc.freezeEndTick = Int(r, "freeze_end_tick", 0);
  sc.length = Int(r, "length", 0);
  if (sc.length <= 0) return Fail(err, "length must be > 0");
  sc.defaultStart = std::clamp(Int(r, "default_start", 0), 0, sc.length - 1);
  sc.winner = Str(r, "winner");
  sc.reason = Str(r, "reason");
  if (const Value* s = Get(r, "source"); s && s->type == VT::Object) {
    sc.source.event = Str(*s, "event");
    sc.source.match = Str(*s, "match");
    sc.source.url = Str(*s, "url");
    sc.source.demo = Str(*s, "demo");
    sc.source.note = Str(*s, "note");
    if (const Value* t = Get(*s, "teams"); t && t->type == VT::Object) {
      sc.source.teamT = Str(*t, "T");
      sc.source.teamCT = Str(*t, "CT");
    }
  }

  const auto* players = Arr(r, "players");
  if (!players || players->empty() || players->size() > static_cast<size_t>(kMaxPlayers)) {
    return Fail(err, "players: 1.." + std::to_string(kMaxPlayers) + " entries");
  }
  for (size_t i = 0; i < players->size(); ++i) {
    const Value& pv = (*players)[i];
    const std::string at = "player " + std::to_string(i + 1) + ": ";
    if (pv.type != VT::Object) return Fail(err, at + "not an object");
    Player p;
    p.name = Str(pv, "name");
    p.steamid = Str(pv, "steamid");
    const std::string team = Str(pv, "team");
    p.team = team == "T" ? 2 : team == "CT" ? 3 : 0;
    if (!p.team) return Fail(err, at + "team must be T or CT");
    p.aliveUntil = Int(pv, "alive_until", -1);
    const auto* tr = Arr(pv, "track");
    if (!tr || tr->size() < 5 || tr->size() % 5) return Fail(err, at + "track must be groups of 5 numbers");
    p.track.reserve(tr->size());
    for (const Value& v : *tr) {
      if (v.type != VT::Number || !std::isfinite(v.num) || std::fabs(v.num) >= 65536) {
        return Fail(err, at + "track value out of range");
      }
      p.track.push_back(static_cast<float>(v.num));
    }
    const auto* st = Arr(pv, "state");
    if (!st || st->empty()) return Fail(err, at + "state missing");
    for (const Value& sv : *st) {
      if (sv.type != VT::Object) return Fail(err, at + "state entry not an object");
      State s;
      s.t = Int(sv, "t", -1);
      s.hp = std::clamp(Int(sv, "hp", 100), 1, 100);
      s.armor = std::clamp(Int(sv, "armor", 0), 0, 100);
      s.helmet = Bool(sv, "helmet");
      s.defuser = Bool(sv, "defuser");
      s.money = std::clamp(Int(sv, "money", 0), 0, 1000000);
      if (const auto* items = Arr(sv, "items")) {
        for (const Value& it : *items) {
          if (it.type == VT::String && it.str.rfind("weapon_", 0) == 0 && it.str.size() < 48) s.items.push_back(it.str);
        }
      }
      if (s.t < 0 || (!p.states.empty() && s.t < p.states.back().t)) return Fail(err, at + "state t must ascend");
      p.states.push_back(std::move(s));
    }
    if (p.states.front().t != 0) return Fail(err, at + "state must start at t=0");
    sc.players.push_back(std::move(p));
  }
  const int np = static_cast<int>(sc.players.size());

  if (const auto* gs = Arr(r, "grenades")) {
    for (const Value& gv : *gs) {
      Grenade g;
      g.t = Int(gv, "t", -1);
      g.player = Int(gv, "player", -1);
      g.landT = Int(gv, "land_t", g.t);
      if (!ParseType(Str(gv, "type"), &g.type)) return Fail(err, "grenade type \"" + Str(gv, "type") + "\"");
      if (g.player < 0 || g.player >= np) return Fail(err, "grenade player index out of range");
      if (!Vec(gv, "pos", &g.pos) || !Vec(gv, "vel", &g.vel) || !Vec(gv, "land", &g.land)) {
        return Fail(err, "grenade pos / vel / land need 3 numbers");
      }
      if (g.t < 0) return Fail(err, "grenade t < 0");
      sc.grenades.push_back(g);
    }
  }
  if (const auto* ds = Arr(r, "deaths")) {
    for (const Value& dv : *ds) {
      Death d;
      d.t = Int(dv, "t", -1);
      d.player = Int(dv, "player", -1);
      d.killer = Int(dv, "killer", -1);
      d.weapon = Str(dv, "weapon");
      if (d.player < 0 || d.player >= np || d.t < 0) return Fail(err, "death player index / t out of range");
      if (d.killer >= np) d.killer = -1;
      sc.deaths.push_back(d);
    }
  }
  if (const auto* bs = Arr(r, "bomb")) {
    for (const Value& bv : *bs) {
      BombEvent b;
      b.t = Int(bv, "t", -1);
      b.event = Str(bv, "event");
      b.player = Int(bv, "player", -1);
      if (b.player >= np) b.player = -1;
      b.hasPos = Vec(bv, "pos", &b.pos);
      if (b.t < 0 || b.event.empty()) continue;
      sc.bomb.push_back(b);
    }
  }
  auto byT = [](const auto& a, const auto& b) { return a.t < b.t; };
  std::stable_sort(sc.grenades.begin(), sc.grenades.end(), byT);
  std::stable_sort(sc.deaths.begin(), sc.deaths.end(), byT);
  std::stable_sort(sc.bomb.begin(), sc.bomb.end(), byT);
  *out = std::move(sc);
  return true;
}

bool ParseFile(const std::string& path, Scenario* out, std::string* err) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return Fail(err, "cannot open " + path);
  std::stringstream ss;
  ss << in.rdbuf();
  if (ss.str().size() > 32u * 1024 * 1024) return Fail(err, "file larger than 32 MiB");
  return Parse(ss.str(), out, err);
}

const char* GrenadeName(GrenadeType t) {
  switch (t) {
    case GrenadeType::kSmoke: return "smoke";
    case GrenadeType::kFlash: return "flash";
    case GrenadeType::kHe: return "HE";
    case GrenadeType::kMolotov: return "molotov";
    case GrenadeType::kIncendiary: return "incendiary";
    case GrenadeType::kDecoy: return "decoy";
  }
  return "?";
}

int GrenadeDefIndex(GrenadeType t) {
  switch (t) {
    case GrenadeType::kFlash: return 43;
    case GrenadeType::kHe: return 44;
    case GrenadeType::kSmoke: return 45;
    case GrenadeType::kMolotov: return 46;
    case GrenadeType::kDecoy: return 47;
    case GrenadeType::kIncendiary: return 48;
  }
  return 0;
}

bool ParseStart(const std::string& text, const Scenario& sc, int* out, std::string* err) {
  std::string s = Lower(text);
  long t = -1;
  auto digits = [](const std::string& x) {
    return !x.empty() && std::all_of(x.begin(), x.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
  };
  if (s.size() > 1 && (s.front() == 't' || s.back() == 't')) {
    const std::string d = s.front() == 't' ? s.substr(1) : s.substr(0, s.size() - 1);
    if (!digits(d) || d.size() > 9) return Fail(err, "bad tick \"" + text + "\" (t12345)");
    t = std::stol(d) - sc.freezeEndTick;
  } else {
    if (!s.empty() && s.back() == 's') s.pop_back();
    double secs = -1;
    const size_t colon = s.find(':');
    char* end = nullptr;
    if (colon != std::string::npos) {
      const std::string m = s.substr(0, colon), rest = s.substr(colon + 1);
      if (!digits(m) || m.size() > 4 || rest.empty()) return Fail(err, "bad time \"" + text + "\" (1:05)");
      const double sec = std::strtod(rest.c_str(), &end);
      if (*end || sec < 0 || sec >= 60) return Fail(err, "bad time \"" + text + "\" (1:05)");
      secs = std::stoi(m) * 60 + sec;
    } else {
      if (s.empty()) return Fail(err, "empty start");
      secs = std::strtod(s.c_str(), &end);
      if (*end || !std::isfinite(secs) || secs < 0) {
        return Fail(err, "bad start \"" + text + "\" (seconds like 25 / 1:05, or a demo tick like t12345)");
      }
    }
    t = std::lround(secs * sc.tickrate);
  }
  if (t < 0 || t >= sc.length) {
    return Fail(err, "start " + text + " is outside the recording (0:00-" + Clock(sc.length, sc.tickrate) + ")");
  }
  *out = static_cast<int>(t);
  return true;
}

int FindPlayer(const Scenario& sc, const std::string& query, std::string* err) {
  const std::string q = Lower(query);
  const int n = static_cast<int>(sc.players.size());
  if (!q.empty() && q.size() <= 2 && std::all_of(q.begin(), q.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
    const int i = std::atoi(q.c_str());
    if (i >= 1 && i <= n) return i - 1;
    Fail(err, "player number 1-" + std::to_string(n));
    return -1;
  }
  for (int i = 0; i < n; ++i) {
    if (!sc.players[i].steamid.empty() && sc.players[i].steamid == query) return i;
  }
  for (int pass = 0; pass < 3; ++pass) {
    std::vector<int> hits;
    for (int i = 0; i < n; ++i) {
      const std::string name = Lower(sc.players[i].name);
      const bool hit = pass == 0 ? name == q : pass == 1 ? name.rfind(q, 0) == 0 : name.find(q) != std::string::npos;
      if (hit) hits.push_back(i);
    }
    if (hits.size() == 1) return hits[0];
    if (hits.size() > 1) {
      std::string names;
      for (int h : hits) names += (names.empty() ? "" : ", ") + sc.players[h].name;
      Fail(err, "\"" + query + "\" matches " + names);
      return -1;
    }
  }
  Fail(err, "no player \"" + query + "\" (" + PlayerList(sc) + ")");
  return -1;
}

int DefaultPlayer(const Scenario& sc, int team, int startT) {
  for (int pass = 0; pass < 2; ++pass) {
    for (size_t i = 0; i < sc.players.size(); ++i) {
      const Player& p = sc.players[i];
      if ((pass == 1 || p.team == team) && p.AliveAt(startT)) return static_cast<int>(i);
    }
  }
  return -1;
}

void BotCounts(const Scenario& sc, int human, int startT, int* t, int* ct) {
  *t = *ct = 0;
  for (size_t i = 0; i < sc.players.size(); ++i) {
    const Player& p = sc.players[i];
    if (static_cast<int>(i) == human || !p.AliveAt(startT)) continue;
    (p.team == 2 ? *t : *ct) += 1;
  }
}

std::vector<Due> DueBetween(const Scenario& sc, int prevT, int nowT) {
  struct E {
    int t;
    Due d;
  };
  std::vector<E> e;
  for (size_t i = 0; i < sc.grenades.size(); ++i) {
    if (sc.grenades[i].t > prevT && sc.grenades[i].t <= nowT) e.push_back({sc.grenades[i].t, {Due::kGrenade, static_cast<int>(i)}});
  }
  for (size_t i = 0; i < sc.deaths.size(); ++i) {
    if (sc.deaths[i].t > prevT && sc.deaths[i].t <= nowT) e.push_back({sc.deaths[i].t, {Due::kDeath, static_cast<int>(i)}});
  }
  for (size_t i = 0; i < sc.bomb.size(); ++i) {
    if (sc.bomb[i].t > prevT && sc.bomb[i].t <= nowT) e.push_back({sc.bomb[i].t, {Due::kBomb, static_cast<int>(i)}});
  }
  std::stable_sort(e.begin(), e.end(), [](const E& a, const E& b) { return a.t < b.t; });
  std::vector<Due> out;
  out.reserve(e.size());
  for (const E& x : e) out.push_back(x.d);
  return out;
}

std::string Clock(int ticks, int tickrate) {
  const int s = tickrate > 0 ? std::max(0, ticks) / tickrate : 0;
  char b[16];
  std::snprintf(b, sizeof(b), "%d:%02d", s / 60, s % 60);
  return b;
}

std::string Summary(const Scenario& sc) {
  std::string s = sc.map + " r" + std::to_string(sc.round);
  if (!sc.source.teamT.empty() || !sc.source.teamCT.empty()) {
    s += " - " + (sc.source.teamT.empty() ? std::string("?") : sc.source.teamT) + " (T) vs " +
         (sc.source.teamCT.empty() ? std::string("?") : sc.source.teamCT) + " (CT)";
  }
  s += " - " + Clock(sc.length, sc.tickrate);
  if (!sc.winner.empty()) s += ", " + sc.winner + " won";
  return s;
}

std::string PlayerList(const Scenario& sc) {
  std::string s;
  for (size_t i = 0; i < sc.players.size(); ++i) {
    s += (i ? ", " : "") + std::to_string(i + 1) + " " + sc.players[i].name + (sc.players[i].team == 2 ? " (T)" : " (CT)");
  }
  return s;
}

std::string Attribution(const Scenario& sc) {
  std::string s;
  for (const std::string* p : {&sc.source.match, &sc.source.event, &sc.source.url}) {
    if (!p->empty()) s += (s.empty() ? "" : " - ") + *p;
  }
  return s.empty() ? "no source given" : s;
}

}  // namespace practice::scenario
