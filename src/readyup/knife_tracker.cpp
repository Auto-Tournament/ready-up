#include "readyup/knife_tracker.h"

#include "readyup/logging.h"
#include "readyup/slot_registry.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace readyup {
namespace {

struct Member {
  int team = 0;
  bool dead = false;
  int health = 100;
};

std::atomic<bool> g_active{false};
std::mutex g_mu;
std::unordered_map<int, Member> g_members;  // keyed by log userid

// `Name<userid><id><TEAM>` -> userid + team (2/3, else 0). False if not a player header.
static bool ParseHeader(const std::string& header, int& userid, int& team) {
  userid = -1;
  team = 0;
  if (header.empty() || header.back() != '>') return false;
  // Walk the trailing <..> groups from the right: [team] [id] [userid].
  std::string groups[3];
  int n = 0;
  size_t end = header.size();
  while (n < 3 && end > 0 && header[end - 1] == '>') {
    const size_t lt = header.rfind('<', end - 1);
    if (lt == std::string::npos) break;
    groups[n++] = header.substr(lt + 1, end - 1 - (lt + 1));
    end = lt;
  }
  if (n < 3) return false;
  const std::string& teamStr = groups[0];
  const std::string& uid = groups[2];
  if (uid.empty() || uid.find_first_not_of("0123456789") != std::string::npos) return false;
  userid = std::atoi(uid.c_str());
  if (teamStr == "CT") team = 3;
  else if (teamStr == "TERRORIST") team = 2;
  return true;
}

// Quoted string starting at/after `from`; returns position after the closing quote.
static bool NextQuoted(const std::string& s, size_t from, std::string& out, size_t& after) {
  const size_t a = s.find('"', from);
  if (a == std::string::npos) return false;
  const size_t b = s.find('"', a + 1);
  if (b == std::string::npos) return false;
  out = s.substr(a + 1, b - a - 1);
  after = b + 1;
  return true;
}

}  // namespace

void KnifeTrackerReset(bool active) {
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_members.clear();
  }
  g_active.store(active, std::memory_order_release);
}

bool KnifeTrackerActive() {
  return g_active.load(std::memory_order_acquire);
}

void KnifeTrackerObserveLine(const std::string& line) {
  if (!KnifeTrackerActive()) return;

  // Attacker header, then an optional `[x y z]` position, then the verb.
  const size_t killed = line.find(" killed \"");
  const size_t attacked = killed == std::string::npos ? line.find(" attacked \"") : std::string::npos;
  const size_t suicide =
      (killed == std::string::npos && attacked == std::string::npos) ? line.find(" committed suicide") : std::string::npos;
  if (killed == std::string::npos && attacked == std::string::npos && suicide == std::string::npos) return;

  std::string first;
  size_t after = 0;
  if (!NextQuoted(line, 0, first, after)) return;

  int uid = -1, team = 0;
  if (suicide != std::string::npos) {
    if (!ParseHeader(first, uid, team)) return;
    std::lock_guard<std::mutex> lk(g_mu);
    auto& m = g_members[uid];
    if (team) m.team = team;
    m.dead = true;
    m.health = 0;
    Debug("knife-tracker: suicide userid=%d team=%d\n", uid, team);
    return;
  }

  // Victim is the second quoted string (after ` killed ` / ` attacked `).
  const size_t victimFrom = (killed != std::string::npos ? killed : attacked) + 1;
  std::string victim;
  if (!NextQuoted(line, victimFrom, victim, after)) return;
  if (!ParseHeader(victim, uid, team)) return;

  std::lock_guard<std::mutex> lk(g_mu);
  auto& m = g_members[uid];
  if (team) m.team = team;
  if (killed != std::string::npos) {
    m.dead = true;
    m.health = 0;
    Debug("knife-tracker: death userid=%d team=%d\n", uid, team);
    return;
  }
  // (health "N")
  const size_t h = line.find("(health \"", after);
  if (h == std::string::npos) return;
  const int hp = std::atoi(line.c_str() + h + 9);
  m.health = std::max(0, std::min(100, hp));
  if (m.health == 0) m.dead = true;
}

KnifeSideStats KnifeTrackerStats(int csTeam) {
  KnifeSideStats out;
  // Current members of that side (log-derived; bots included).
  std::vector<int> uids;
  for (const auto& h : ListHumans()) {
    if (h.team == csTeam && h.userid >= 0) uids.push_back(h.userid);
  }
  for (const auto& b : ListBots()) {
    if (b.team == csTeam && b.userid >= 0) uids.push_back(b.userid);
  }
  std::lock_guard<std::mutex> lk(g_mu);
  for (int uid : uids) {
    out.members++;
    auto it = g_members.find(uid);
    if (it == g_members.end()) {
      out.alive++;
      out.hp += 100;
      continue;
    }
    if (it->second.dead) continue;
    out.alive++;
    out.hp += it->second.health;
  }
  return out;
}

}  // namespace readyup
