// The end-of-map / end-of-series leaderboard on the Auto Tournament HUD (see end_screen.h).
#include "readyup/end_screen.h"

#include "readyup/host.h"
#include "readyup/hud_iface.h"
#include "readyup/logging.h"
#include "readyup/players.h"
#include "readyup/plugin_api.h"

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <set>

namespace readyup {
namespace {

constexpr const char* kKitLayout = "panorama/layout/custom_game/at_kit.xml";
constexpr int kRoundSlots = 30;  // hud/tools/gen_kit.py END_ROUNDS
constexpr int kRows = 5;

// Engine round_end reasons (CS2 RoundEndReason): how the strip letters a round.
char HowOf(int reason) {
  switch (reason) {
    case 1: return 'B';   // target bombed
    case 7: return 'D';   // bomb defused
    case 12: return 'T';  // target saved (time ran out)
    case 13: return 'T';  // hostages not rescued
    default: return 'E';  // eliminations (and anything else)
  }
}

int Pct(int part, int whole) { return whole > 0 ? (part * 100 + whole / 2) / whole : 0; }

struct State {
  bool active = false;
  double until = 0;
  int lastSecs = -1;
  std::string nextLabel, next;
  std::set<int> captured;  // slots holding the cursor
};
State g;
std::mutex g_mu;

const ru_hud_v1* Hud() {
  const ru_api* a = host::Api();
  if (!a) return nullptr;
  const auto* h = static_cast<const ru_hud_v1*>(a->get_interface(a->self, RU_HUD_IFACE_NAME, 1));
  if (!h || !h->available || !h->available() || !h->open || !h->set_text || !h->set_class) return nullptr;
  return h;
}

bool HasCapture(const ru_hud_v1* h) {
  return h->struct_size >= offsetof(ru_hud_v1, set_capture) + sizeof(h->set_capture) && h->set_capture;
}

std::string Footer(const std::string& label, const std::string& next, int secs) {
  std::string s = label;
  if (!next.empty()) s += " " + next;
  return s + " (" + std::to_string(std::max(0, secs)) + ")";
}

}  // namespace

EndScreenData BuildEndScreenData(const EndScreenInfo& info) {
  EndScreenData out;
  const auto& st = info.stats;
  for (const auto& p : st.players) {
    if (p.team != 1 && p.team != 2) continue;
    const auto& s = p.stats;
    if (s.rounds_played == 0 && s.kills == 0 && s.deaths == 0) continue;  // never played this map
    EndScreenRow r;
    r.name = p.name;
    r.id = p.id;
    r.k = s.kills;
    r.d = s.deaths;
    r.a = s.assists + s.flash_assists;
    r.adr = s.rounds_played > 0 ? (s.damage + s.rounds_played / 2) / s.rounds_played : 0;
    r.kast = Pct(s.kast_rounds, s.rounds_played);
    r.hs = Pct(s.headshot_kills, s.kills);
    r.ud = s.utility_damage;
    r.ek = s.entry_kills_t + s.entry_kills_ct;
    r.mvps = s.mvp;
    (p.team == 1 ? out.team1 : out.team2).push_back(std::move(r));
  }
  auto order = [](std::vector<EndScreenRow>& v) {
    std::stable_sort(v.begin(), v.end(), [](const EndScreenRow& a, const EndScreenRow& b) {
      if (a.k != b.k) return a.k > b.k;
      return a.adr > b.adr;
    });
    if (v.size() > static_cast<size_t>(kRows)) v.resize(kRows);
  };
  order(out.team1);
  order(out.team2);

  // Awards: the leader of each stat over both teams (the first one on a tie). Nobody scored: none.
  std::vector<const EndScreenRow*> all;
  for (const auto& r : out.team1) all.push_back(&r);
  for (const auto& r : out.team2) all.push_back(&r);
  auto award = [&](const char* label, int EndScreenRow::*field, const char* unit) {
    const EndScreenRow* best = nullptr;
    for (const auto* r : all) {
      if (!best || r->*field > best->*field) best = r;
    }
    if (best && best->*field > 0) out.awards.push_back({label, best->name, std::to_string(best->*field) + unit});
  };
  award("TOP FRAGGER", &EndScreenRow::k, " K");
  award("MOST DAMAGE", &EndScreenRow::adr, " ADR");
  award("ENTRY KILLS", &EndScreenRow::ek, "");
  award("UTILITY DAMAGE", &EndScreenRow::ud, "");

  // Round strip, with a gap at every half: regulation, then each overtime's halves.
  const int regHalf = std::max(1, info.maxRounds / 2);
  const int otHalf = std::max(1, info.overtimeRounds / 2);
  int n = 0;
  for (const auto& rs : st.rounds) {
    if (n >= kRoundSlots) break;
    EndScreenRound r;
    r.winner = rs.winner_team;
    r.how = HowOf(rs.reason);
    r.halfBefore = n == regHalf || (n > info.maxRounds && (n - info.maxRounds) % otHalf == 0) || n == info.maxRounds;
    out.rounds.push_back(r);
    ++n;
  }
  const char* dot = " \xC2\xB7 ";
  out.sides1 = "T " + std::to_string(st.team1.score_t) + dot + "CT " + std::to_string(st.team1.score_ct);
  out.sides2 = "T " + std::to_string(st.team2.score_t) + dot + "CT " + std::to_string(st.team2.score_ct);
  return out;
}

bool EndScreenShow(const EndScreenInfo& info) {
  const ru_hud_v1* h = Hud();
  if (!h) return false;
  const int k = h->open(kKitLayout);
  if (k <= 0) return false;
  const EndScreenData d = BuildEndScreenData(info);
  const int ALL = RU_HUD_ALL;
  auto txt = [&](int slot, const std::string& panel, const std::string& v) { h->set_text(k, slot, panel.c_str(), "text", v.c_str()); };
  auto cls = [&](int slot, const std::string& panel, const char* c, bool on) { h->set_class(k, slot, panel.c_str(), c, on ? 1 : 0); };

  // Everyone's: names, score, how, rows, strip, awards, footer.
  for (const char* el : {"at_card", "at_mid"}) cls(ALL, el, "on", false);
  txt(ALL, "at_e_ta", info.team1);
  txt(ALL, "at_e_tb", info.team2);
  txt(ALL, "at_e_sa", std::to_string(info.score1));
  txt(ALL, "at_e_sb", std::to_string(info.score2));
  txt(ALL, "at_e_sidea", d.sides1);
  txt(ALL, "at_e_sideb", d.sides2);
  txt(ALL, "at_e_how", info.how);
  cls(ALL, "at_end", "how", !info.how.empty());
  cls(ALL, "at_end", "wa", info.winner == "team1");
  cls(ALL, "at_end", "wb", info.winner == "team2");
  txt(ALL, "at_ea_name", info.team1);
  txt(ALL, "at_eb_name", info.team2);
  auto rows = [&](char side, const std::vector<EndScreenRow>& v) {
    for (int i = 0; i < kRows; ++i) {
      const std::string r = std::string("at_e") + side + std::to_string(i);
      const bool has = i < static_cast<int>(v.size());
      cls(ALL, r, "empty", !has);
      if (!has) continue;
      const EndScreenRow& x = v[static_cast<size_t>(i)];
      const int diff = x.k - x.d;
      txt(ALL, r + "_n", x.name);
      txt(ALL, r + "_m", std::to_string(x.mvps) + " MVP");
      cls(ALL, r, "mvp", x.mvps > 0);
      txt(ALL, r + "_k", std::to_string(x.k));
      txt(ALL, r + "_d", std::to_string(x.d));
      txt(ALL, r + "_a", std::to_string(x.a));
      txt(ALL, r + "_p", (diff > 0 ? "+" : "") + std::to_string(diff));
      cls(ALL, r + "_p", "pos", diff > 0);
      cls(ALL, r + "_p", "neg", diff < 0);
      txt(ALL, r + "_r", std::to_string(x.adr));
      cls(ALL, r + "_r", "top", x.adr >= 100);
      txt(ALL, r + "_s", std::to_string(x.kast) + "%");
      txt(ALL, r + "_h", std::to_string(x.hs) + "%");
      txt(ALL, r + "_u", std::to_string(x.ud));
      txt(ALL, r + "_e", std::to_string(x.ek));
    }
  };
  rows('a', d.team1);
  rows('b', d.team2);
  for (int i = 0; i < kRoundSlots; ++i) {
    const std::string r = "at_rt" + std::to_string(i);
    const bool has = i < static_cast<int>(d.rounds.size());
    cls(ALL, r, "on", has);
    if (!has) continue;
    const EndScreenRound& x = d.rounds[static_cast<size_t>(i)];
    const std::string letter(1, x.how);
    cls(ALL, r, "a", x.winner == 1);
    cls(ALL, r, "b", x.winner == 2);
    cls(ALL, r, "half", x.halfBefore);
    txt(ALL, r + "_u", x.winner == 1 ? letter : "");
    txt(ALL, r + "_d", x.winner == 2 ? letter : "");
  }
  cls(ALL, "at_erounds", "none", d.rounds.empty());
  for (int i = 0; i < 4; ++i) {
    const std::string a = "at_aw" + std::to_string(i);
    const bool has = i < static_cast<int>(d.awards.size());
    cls(ALL, a, "empty", !has);
    if (!has) continue;
    txt(ALL, a + "_l", d.awards[static_cast<size_t>(i)].label);
    txt(ALL, a + "_n", d.awards[static_cast<size_t>(i)].name);
    txt(ALL, a + "_v", d.awards[static_cast<size_t>(i)].value);
  }
  txt(ALL, "at_e_where", info.where);
  txt(ALL, "at_e_next", Footer(info.nextLabel, info.next, info.seconds));

  // Each player's own view: the headline, their row, their team's rounds, the cursor.
  std::lock_guard<std::mutex> lk(g_mu);
  g = State{};
  g.active = true;
  g.until = host::NowSeconds() + info.seconds;
  g.lastSecs = info.seconds;
  g.nextLabel = info.nextLabel;
  g.next = info.next;
  for (const auto& hu : ListHumans()) {
    const int slot = hu.slot;
    if (hu.steamid64 == 0 || slot < 0 || slot > 63) continue;
    int myTeam = 0;
    for (const auto& p : info.stats.players) {
      if (p.id == hu.steamid64) myTeam = p.team;
    }
    std::string head;
    const char* state = "draw";
    if (info.winner.empty()) {
      head = "Draw";
    } else if (myTeam == 1 || myTeam == 2) {
      const bool won = (myTeam == 1) == (info.winner == "team1");
      head = won ? "You won!" : "You lost";
      state = won ? "won" : "lost";
    } else {
      head = (info.winner == "team1" ? info.team1 : info.team2) + " won";
      state = "won";
    }
    txt(slot, "at_e_head", head);
    for (const char* c : {"won", "lost", "draw"}) cls(slot, "at_end", c, std::string(c) == state);
    cls(slot, "at_end", "viewa", myTeam == 1);
    cls(slot, "at_end", "viewb", myTeam == 2);
    for (int t = 0; t < 2; ++t) {
      const auto& v = t == 0 ? d.team1 : d.team2;
      for (int i = 0; i < kRows; ++i) {
        const bool me = i < static_cast<int>(v.size()) && v[static_cast<size_t>(i)].id == hu.steamid64;
        cls(slot, std::string("at_e") + (t == 0 ? 'a' : 'b') + std::to_string(i), "me", me);
      }
    }
    if (HasCapture(h) && h->set_capture(k, slot, 1)) g.captured.insert(slot);
  }
  cls(ALL, "at_end", "on", true);
  Print("end-screen: shown for %ds (%zu + %zu rows, %zu rounds)\n", info.seconds, d.team1.size(), d.team2.size(),
        d.rounds.size());
  return true;
}

void EndScreenHide() {
  std::set<int> captured;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g.active) return;
    captured.swap(g.captured);
    g = State{};
  }
  const ru_hud_v1* h = Hud();
  if (!h) return;
  const int k = h->open(kKitLayout);
  if (k <= 0) return;
  h->set_class(k, RU_HUD_ALL, "at_end", "on", 0);
  if (HasCapture(h)) {
    for (int slot : captured) h->set_capture(k, slot, 0);
  }
}

void EndScreenTick() {
  int secs = -1;
  std::string label, next;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g.active) return;
    const double left = g.until - host::NowSeconds();
    if (left <= 0) {
      secs = -2;
    } else {
      const int s = static_cast<int>(left + 0.99);
      if (s == g.lastSecs) return;
      g.lastSecs = s;
      secs = s;
      label = g.nextLabel;
      next = g.next;
    }
  }
  if (secs == -2) return EndScreenHide();
  const ru_hud_v1* h = Hud();
  if (!h) return;
  const int k = h->open(kKitLayout);
  if (k > 0) h->set_text(k, RU_HUD_ALL, "at_e_next", "text", Footer(label, next, secs).c_str());
}

bool EndScreenActive() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g.active;
}

}  // namespace readyup
