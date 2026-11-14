#include "retakes_rules.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace retakes {
namespace {

uint32_t Le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t Le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

std::string Lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// A null-terminated string at *pos; false at the end of the buffer.
bool CString(const uint8_t* b, size_t n, size_t* pos, std::string* out) {
  if (*pos >= n) return false;
  const void* end = std::memchr(b + *pos, 0, n - *pos);
  if (!end) return false;
  const size_t len = static_cast<const uint8_t*>(end) - (b + *pos);
  out->assign(reinterpret_cast<const char*>(b + *pos), len);
  *pos += len + 1;
  return true;
}

// ---- KeyValues (the subset retake files use: quoted keys and values, braces, // comments) ----

struct Kv {
  std::string key, value;
  std::vector<Kv> kids;
  const Kv* Child(const char* k) const {
    for (const auto& c : kids)
      if (Lower(c.key) == Lower(k)) return &c;
    return nullptr;
  }
};

struct Lexer {
  const std::string& s;
  size_t i = 0;
  // Next token: a quoted or bare string, "{" or "}"; "" at the end.
  bool Next(std::string* tok, bool* quoted) {
    for (;;) {
      while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
      if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '/') {
        while (i < s.size() && s[i] != '\n') ++i;
        continue;
      }
      break;
    }
    *quoted = false;
    if (i >= s.size()) return false;
    if (s[i] == '{' || s[i] == '}') {
      *tok = std::string(1, s[i++]);
      return true;
    }
    if (s[i] == '"') {
      const size_t close = s.find('"', i + 1);
      if (close == std::string::npos) return false;
      *tok = s.substr(i + 1, close - i - 1);
      i = close + 1;
      *quoted = true;
      return true;
    }
    const size_t start = i;
    while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])) && s[i] != '{' && s[i] != '}') ++i;
    *tok = s.substr(start, i - start);
    return true;
  }
};

// Parses key/value pairs until "}" (or the end, at depth 0).
bool ParseBlock(Lexer& lx, std::vector<Kv>* out, int depth) {
  std::string tok;
  bool quoted = false;
  while (lx.Next(&tok, &quoted)) {
    if (!quoted && tok == "}") return depth > 0;
    if (!quoted && tok == "{") return false;
    Kv kv;
    kv.key = tok;
    std::string val;
    if (!lx.Next(&val, &quoted)) return false;
    if (!quoted && val == "{") {
      if (depth > 16 || !ParseBlock(lx, &kv.kids, depth + 1)) return false;
    } else if (!quoted && val == "}") {
      return false;
    } else {
      kv.value = val;
    }
    out->push_back(std::move(kv));
  }
  return depth == 0;
}

bool ParseSpawn(const std::string& v, Spawn* s) {
  const char* p = v.c_str();
  float f[6];
  for (float& x : f) {
    char* end = nullptr;
    x = std::strtof(p, &end);
    if (end == p || !std::isfinite(x)) return false;
    p = end;
  }
  std::copy(f, f + 3, s->pos);
  std::copy(f + 3, f + 6, s->ang);
  return true;
}

// Permutes `ids` with the caller's shuffle.
void ShuffleIds(std::vector<uint64_t>& ids, const Shuffle& shuffle) {
  std::vector<size_t> idx(ids.size());
  for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
  if (shuffle) shuffle(idx);
  std::vector<uint64_t> out;
  out.reserve(ids.size());
  for (size_t i : idx) out.push_back(ids[i]);
  ids.swap(out);
}

}  // namespace

uint32_t VpkTreeSize(const uint8_t* h, size_t n) {
  if (n < kVpkHeaderV2 || Le32(h) != 0x55aa1234u || Le32(h + 4) != 2) return 0;
  return Le32(h + 8);
}

bool VpkFind(const uint8_t* tree, size_t n, const std::string& want, VpkEntry* out) {
  const std::string path = Lower(want);
  size_t pos = 0;
  std::string ext, dir, name;
  for (;;) {
    if (!CString(tree, n, &pos, &ext)) return false;
    if (ext.empty()) return false;
    for (;;) {
      if (!CString(tree, n, &pos, &dir)) return false;
      if (dir.empty()) break;
      for (;;) {
        if (!CString(tree, n, &pos, &name)) return false;
        if (name.empty()) break;
        if (pos + 18 > n) return false;
        VpkEntry e;
        const uint16_t preload = Le16(tree + pos + 4);
        e.archive = Le16(tree + pos + 6);
        e.offset = Le32(tree + pos + 8);
        e.length = Le32(tree + pos + 12);
        if (Le16(tree + pos + 16) != 0xffff) return false;
        pos += 18;
        if (pos + preload > n) return false;
        e.preload.assign(reinterpret_cast<const char*>(tree + pos), preload);
        pos += preload;
        const std::string full = (dir == " " ? "" : dir + "/") + name + (ext == " " ? "" : "." + ext);
        if (Lower(full) == path) {
          *out = std::move(e);
          return true;
        }
      }
    }
  }
}

bool MapSpawns::Usable() const {
  for (const auto& s : site)
    if (s.ct.empty() || s.t.empty()) return false;
  return true;
}

bool ParseRetakeFile(const std::string& text, MapSpawns* out, std::string* err) {
  Lexer lx{text};
  std::vector<Kv> root;
  if (!ParseBlock(lx, &root, 0)) {
    if (err) *err = "not a KeyValues file";
    return false;
  }
  const Kv* retake = nullptr;
  for (const auto& k : root)
    if (Lower(k.key) == "retake") retake = &k;
  const Kv* spawns = retake ? retake->Child("Spawns") : nullptr;
  if (!spawns) {
    if (err) *err = "no \"Retake\" > \"Spawns\"";
    return false;
  }
  MapSpawns m;
  for (const char* team : {"CT", "T"}) {
    const Kv* t = spawns->Child(team);
    if (!t) continue;
    for (int site = 0; site < 2; ++site) {
      const Kv* s = t->Child(site == 0 ? "0" : "1");
      if (!s) continue;
      auto& list = std::strcmp(team, "CT") == 0 ? m.site[site].ct : m.site[site].t;
      for (const auto& e : s->kids) {
        Spawn sp;
        if (!ParseSpawn(e.value, &sp)) {
          if (err) *err = std::string("bad spawn \"") + e.value.substr(0, 60) + "\"";
          return false;
        }
        list.push_back(sp);
      }
    }
  }
  *out = std::move(m);
  return true;
}

int TargetTerrorists(int players) {
  if (players < 2) return 0;
  if (players % 10 == 0) return players / 2;
  return std::max(1, static_cast<int>(std::lround(0.45 * players)));
}

Teams NextTeams(const std::vector<Player>& players, int lastWinner, int maxPlayers, int scrambleAfter,
                Streak* streak, const Shuffle& shuffle) {
  Teams out;
  std::vector<const Player*> active, queue;
  for (const auto& p : players) (p.queued ? queue : active).push_back(&p);
  std::stable_sort(queue.begin(), queue.end(), [](const Player* a, const Player* b) { return a->order < b->order; });
  // Too many playing (max lowered): the newest go back to the queue.
  while (static_cast<int>(active.size()) > maxPlayers) {
    queue.insert(queue.begin(), active.back());
    active.pop_back();
  }
  std::vector<const Player*> joining;
  size_t q = 0;
  for (; q < queue.size() && static_cast<int>(active.size() + joining.size()) < maxPlayers; ++q)
    joining.push_back(queue[q]);
  for (; q < queue.size(); ++q) out.queued.push_back(queue[q]->id);

  const int n = static_cast<int>(active.size() + joining.size());
  const int target = TargetTerrorists(n);
  auto byScore = [](std::vector<const Player*> v) {
    std::stable_sort(v.begin(), v.end(), [](const Player* a, const Player* b) { return a->score > b->score; });
    return v;
  };
  std::vector<const Player*> oldT, oldCT;
  for (const auto* p : active) (p->team == kT ? oldT : p->team == kCT ? oldCT : joining).push_back(p);

  std::vector<uint64_t> t, ct;
  if (lastWinner == kT && streak) ++streak->tWins;
  if (lastWinner == kCT && streak) streak->tWins = 0;
  if (lastWinner == kT && scrambleAfter > 0 && streak && streak->tWins >= scrambleAfter) {
    std::vector<uint64_t> all;
    for (const auto* p : oldT) all.push_back(p->id);
    for (const auto* p : oldCT) all.push_back(p->id);
    for (const auto* p : joining) all.push_back(p->id);
    ShuffleIds(all, shuffle);
    t.assign(all.begin(), all.begin() + std::min<size_t>(all.size(), static_cast<size_t>(target)));
    ct.assign(all.begin() + static_cast<long>(t.size()), all.end());
    streak->tWins = 0;
    out.scrambled = true;
    out.t = std::move(t);
    out.ct = std::move(ct);
    return out;
  }
  if (lastWinner == kCT) {
    // The CTs who did the most move to T; the Ts who lost defend next.
    std::vector<uint64_t> rest;
    for (const auto* p : byScore(oldCT)) {
      if (p->score > 0 && static_cast<int>(t.size()) < target) t.push_back(p->id);
      else rest.push_back(p->id);
    }
    ShuffleIds(rest, shuffle);
    for (uint64_t id : rest) (static_cast<int>(t.size()) < target ? t : ct).push_back(id);
    for (const auto* p : byScore(oldT)) (static_cast<int>(t.size()) < target ? t : ct).push_back(p->id);
  } else {
    for (const auto* p : oldT) t.push_back(p->id);
    for (const auto* p : oldCT) ct.push_back(p->id);
  }
  // New players fill the side that is short, then the counts are evened out.
  for (const auto* p : joining) (static_cast<int>(t.size()) < target ? t : ct).push_back(p->id);
  while (static_cast<int>(t.size()) > target) {
    ct.push_back(t.back());
    t.pop_back();
  }
  if (static_cast<int>(t.size()) < target) {
    ShuffleIds(ct, shuffle);
    while (static_cast<int>(t.size()) < target && !ct.empty()) {
      t.push_back(ct.back());
      ct.pop_back();
    }
  }
  out.t = std::move(t);
  out.ct = std::move(ct);
  return out;
}

RoundSpawns PlaceRound(const SiteSpawns& site, const std::vector<uint64_t>& tIn, const std::vector<uint64_t>& ctIn,
                       const std::vector<int>& plantable, const Shuffle& shuffle) {
  RoundSpawns out;
  auto order = [&](size_t n) {
    std::vector<size_t> idx(n);
    for (size_t i = 0; i < n; ++i) idx[i] = i;
    if (shuffle) shuffle(idx);
    return std::vector<int>(idx.begin(), idx.end());
  };
  std::vector<uint64_t> t = tIn, ct = ctIn;
  ShuffleIds(t, shuffle);
  ShuffleIds(ct, shuffle);

  std::vector<int> tPool = order(site.t.size());
  if (!t.empty()) {
    out.planter = t.front();
    // The planter stands on a spawn inside the bombsite when there is one.
    std::vector<int> plant;
    for (int i : plantable)
      if (i >= 0 && i < static_cast<int>(site.t.size())) plant.push_back(i);
    int pick = -1;
    if (!plant.empty()) {
      std::vector<size_t> pi(plant.size());
      for (size_t i = 0; i < pi.size(); ++i) pi[i] = i;
      if (shuffle) shuffle(pi);
      pick = plant[pi.front()];
    } else if (!tPool.empty()) {
      pick = tPool.front();
    }
    if (pick >= 0) tPool.erase(std::find(tPool.begin(), tPool.end(), pick));
    out.t.push_back({t.front(), pick});
    for (size_t i = 1; i < t.size(); ++i) {
      const int s = i - 1 < tPool.size() ? tPool[i - 1] : -1;
      out.t.push_back({t[i], s});
    }
  }
  const std::vector<int> ctPool = order(site.ct.size());
  for (size_t i = 0; i < ct.size(); ++i) out.ct.push_back({ct[i], i < ctPool.size() ? ctPool[i] : -1});
  return out;
}

}  // namespace retakes
