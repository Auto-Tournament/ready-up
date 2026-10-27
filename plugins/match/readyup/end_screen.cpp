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
  bool preview = false;    // .enddemo, not a real end of map
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

namespace {

// The table's columns: header, the hover card, width and how a cell is coloured. The layout has
// kCols slots per table; the rest are switched off. Changing a column is a server change only.
struct Column {
  const char* head;
  const char* tipTitle;
  const char* tipBody;
  const char* width;  // cw-xs cw-s cw-m cw-l cw-xl
  std::string (*value)(const EndScreenRow&);
  const char* (*tone)(const EndScreenRow&);  // a tc-* class
};
constexpr int kCols = 12;  // hud/tools/gen_kit.py END_COLS

const char* Ink(const EndScreenRow&) { return "tc-ink"; }
const char* Soft(const EndScreenRow&) { return "tc-soft"; }
const Column kColumns[] = {
    {"K", "Kills", "Enemies killed this map", "cw-s", [](const EndScreenRow& r) { return std::to_string(r.k); }, Ink},
    {"D", "Deaths", "Times killed this map", "cw-s", [](const EndScreenRow& r) { return std::to_string(r.d); }, Soft},
    {"A", "Assists", "Helped kill an enemy, flashes included", "cw-s",
     [](const EndScreenRow& r) { return std::to_string(r.a); }, Soft},
    {"+/-", "Kills minus deaths", "Above zero: more kills than deaths", "cw-m",
     [](const EndScreenRow& r) { const int d = r.k - r.d; return (d > 0 ? "+" : "") + std::to_string(d); },
     [](const EndScreenRow& r) { return r.k > r.d ? "tc-pos" : r.k < r.d ? "tc-ban" : "tc-muted"; }},
    {"ADR", "Average damage per round", "Damage dealt, divided by rounds played", "cw-m",
     [](const EndScreenRow& r) { return std::to_string(r.adr); },
     [](const EndScreenRow& r) { return r.adr >= 100 ? "tc-acc" : "tc-soft"; }},
    {"KAST", "KAST", "Rounds with a kill, assist, survival or trade", "cw-l",
     [](const EndScreenRow& r) { return std::to_string(r.kast) + "%"; }, Soft},
    {"HS", "Headshots", "Share of kills that were headshots", "cw-m",
     [](const EndScreenRow& r) { return std::to_string(r.hs) + "%"; }, Soft},
    {"UD", "Utility damage", "Damage from HE grenades and fire", "cw-m",
     [](const EndScreenRow& r) { return std::to_string(r.ud); }, Soft},
    {"EK", "Entry kills", "The first kill of a round", "cw-s", [](const EndScreenRow& r) { return std::to_string(r.ek); }, Soft},
};
constexpr int kUsedCols = static_cast<int>(sizeof(kColumns) / sizeof(kColumns[0]));

// The look, as utility classes on #at_end / #at_ecard (hud/styles/at_kit.css, end screen utilities).
constexpr const char* kScale = "scale-85";
// No world blur or dimming: the card is solid on its own, the game stays clear around it (2026-10-27).
constexpr const char* kBlur = "blur-0";
constexpr const char* kDim = "dim-0";
constexpr const char* kBg = "bg-90";  // the card, 90 % opaque (2026-10-27: 75 was too see-through)
// No cursor: players move, look around and type in chat ("gg") while it is up. Hover tooltips did
// not show in CS2, so nothing needs the cursor (2026-10-27).
constexpr bool kHoldCursor = false;

const char* const kTextTones[] = {"tc-ink", "tc-soft", "tc-muted", "tc-acc", "tc-ban", "tc-pos", "tc-live", "tc-warn", "tc-ct", "tc-t"};
const char* const kFills[] = {"fill-none", "fill-acc", "fill-muted", "fill-ban", "fill-live", "fill-ct", "fill-t"};
const char* const kTints[] = {"tint-none", "tint-acc", "tint-ink", "tint-ban", "tint-live", "tint-ct", "tint-t"};
const char* const kTags[] = {"tag-acc", "tag-muted", "tag-live", "tag-ban"};
const char* const kWidthsCw[] = {"cw-xs", "cw-s", "cw-m", "cw-l", "cw-xl", "cw-off"};
const char* const kLines[] = {"line-acc", "line-ban", "line-muted", "line-live"};

struct Writer {
  const ru_hud_v1* h;
  int k;
  void Text(int slot, const std::string& panel, const std::string& v) const { h->set_text(k, slot, panel.c_str(), "text", v.c_str()); }
  void Class(int slot, const std::string& panel, const char* c, bool on) const { h->set_class(k, slot, panel.c_str(), c, on ? 1 : 0); }
  // One class of a family on (keep, or none when null), the others off.
  template <size_t N>
  void Only(int slot, const std::string& panel, const char* const (&family)[N], const char* keep) const {
    for (const char* c : family) Class(slot, panel, c, keep && std::string(c) == keep);
  }
};

}  // namespace

bool EndScreenShow(const EndScreenInfo& info, int target) {
  const ru_hud_v1* h = Hud();
  if (!h) return false;
  const int k = h->open(kKitLayout);
  if (k <= 0) return false;
  const EndScreenData d = BuildEndScreenData(info);
  const Writer w{h, k};
  const int ALL = target;  // everyone, or one player for a preview (.enddemo)

  // Everyone's part: names, score, how, the tables, the strip's letters, the awards, the footer.
  for (const char* el : {"at_card", "at_mid"}) w.Class(ALL, el, "on", false);
  w.Text(ALL, "at_e_ta", info.team1);
  w.Text(ALL, "at_e_tb", info.team2);
  w.Text(ALL, "at_e_sa", std::to_string(info.score1));
  w.Text(ALL, "at_e_sb", std::to_string(info.score2));
  w.Text(ALL, "at_e_sidea", d.sides1);
  w.Text(ALL, "at_e_sideb", d.sides2);
  // The loser's name and score in grey, whoever is watching.
  const bool aLost = info.winner == "team2", bLost = info.winner == "team1";
  for (const char* id : {"at_e_ta", "at_e_sa"}) w.Only(ALL, id, kTextTones, aLost ? "tc-muted" : "tc-ink");
  for (const char* id : {"at_e_tb", "at_e_sb"}) w.Only(ALL, id, kTextTones, bLost ? "tc-muted" : "tc-ink");
  w.Text(ALL, "at_e_how", info.how);
  w.Class(ALL, "at_e_howbox", "hide", info.how.empty());
  for (char side : {'a', 'b'}) {
    const std::string t = std::string("at_e") + side;
    w.Text(ALL, t + "_name", side == 'a' ? info.team1 : info.team2);
    for (int c = 0; c < kCols; ++c) {
      const std::string hc = t + "h_c" + std::to_string(c);
      const bool used = c < kUsedCols;
      w.Only(ALL, hc, kWidthsCw, used ? kColumns[c].width : "cw-off");
      if (!used) continue;
      w.Text(ALL, hc + "_t", kColumns[c].head);
      w.Text(ALL, hc + "_tk", kColumns[c].tipTitle);
      w.Text(ALL, hc + "_tv", kColumns[c].tipBody);
    }
    const auto& v = side == 'a' ? d.team1 : d.team2;
    for (int i = 0; i < 6; ++i) {  // gen_kit.py END_ROWS
      const std::string r = t + std::to_string(i);
      const bool has = i < static_cast<int>(v.size());
      w.Class(ALL, r, "empty", !has);
      if (!has) continue;
      const EndScreenRow& x = v[static_cast<size_t>(i)];
      w.Text(ALL, r + "_n", x.name);
      w.Text(ALL, r + "_g", x.mvps > 0 ? std::to_string(x.mvps) + " MVP" : std::string());
      w.Only(ALL, r + "_g", kTags, x.mvps > 0 ? "tag-acc" : nullptr);
      for (int c = 0; c < kCols; ++c) {
        const std::string cell = r + "_c" + std::to_string(c);
        const bool used = c < kUsedCols;
        w.Only(ALL, cell, kWidthsCw, used ? kColumns[c].width : "cw-off");
        if (!used) continue;
        w.Text(ALL, cell, kColumns[c].value(x));
        w.Only(ALL, cell, kTextTones, kColumns[c].tone(x));
        w.Class(ALL, cell, "bold", c == 0 || kColumns[c].tone(x) == std::string("tc-pos") || kColumns[c].tone(x) == std::string("tc-ban"));
      }
    }
  }
  for (int i = 0; i < kRoundSlots; ++i) {
    const std::string r = "at_rt" + std::to_string(i);
    const bool has = i < static_cast<int>(d.rounds.size());
    w.Class(ALL, r, "on", has);
    if (!has) continue;
    const EndScreenRound& x = d.rounds[static_cast<size_t>(i)];
    const std::string letter(1, x.how);
    w.Class(ALL, r, "half", x.halfBefore);
    w.Text(ALL, r + "_u", x.winner == 1 ? letter : "");
    w.Text(ALL, r + "_d", x.winner == 2 ? letter : "");
  }
  w.Class(ALL, "at_erounds", "hide", d.rounds.empty());
  for (int i = 0; i < 4; ++i) {
    const std::string a = "at_aw" + std::to_string(i);
    const bool has = i < static_cast<int>(d.awards.size());
    w.Class(ALL, a, "empty", !has);
    if (!has) continue;
    w.Text(ALL, a + "_l", d.awards[static_cast<size_t>(i)].label);
    w.Text(ALL, a + "_n", d.awards[static_cast<size_t>(i)].name);
    w.Text(ALL, a + "_v", d.awards[static_cast<size_t>(i)].value);
    w.Only(ALL, a + "_v", kTextTones, "tc-acc");
  }
  w.Class(ALL, "at_eawards", "hide", d.awards.empty());
  w.Text(ALL, "at_e_where", info.where);
  w.Text(ALL, "at_e_next", Footer(info.nextLabel, info.next, info.seconds));
  for (const char* c : {"scale-70", "scale-75", "scale-80", "scale-85", "scale-90", "scale-95", "scale-100"}) {
    w.Class(ALL, "at_end", c, std::string(c) == kScale);
  }
  for (const char* c : {"blur-0", "blur-1", "blur-2", "blur-3"}) w.Class(ALL, "at_end", c, std::string(c) == kBlur);
  for (const char* c : {"dim-0", "dim-1", "dim-2", "dim-3"}) w.Class(ALL, "at_end", c, std::string(c) == kDim);
  for (int p = 0; p <= 100; p += 5) {
    const std::string bg = "bg-" + std::to_string(p);
    w.Class(ALL, "at_ecard", bg.c_str(), bg == kBg);
  }

  // Each player's own view: the headline and its colour, their row, their team's rounds, the cursor.
  std::lock_guard<std::mutex> lk(g_mu);
  if (target == RU_HUD_ALL) {
    g = State{};
    g.active = true;
    g.until = host::NowSeconds() + info.seconds;
    g.lastSecs = info.seconds;
    g.nextLabel = info.nextLabel;
    g.next = info.next;
  }
  for (const auto& hu : ListHumans()) {
    const int slot = hu.slot;
    if (hu.steamid64 == 0 || slot < 0 || slot > 63) continue;
    if (target != RU_HUD_ALL && slot != target) continue;
    int myTeam = 0;
    for (const auto& p : info.stats.players) {
      if (p.id == hu.steamid64) myTeam = p.team;
    }
    if (target != RU_HUD_ALL && myTeam == 0) myTeam = 1;  // a preview: the viewer plays for team 1
    std::string head;
    const char* tone = "tc-muted";
    const char* line = "line-muted";
    if (info.winner.empty()) {
      head = "Draw";
    } else if (myTeam == 1 || myTeam == 2) {
      const bool won = (myTeam == 1) == (info.winner == "team1");
      head = won ? "You won!" : "You lost";
      tone = won ? "tc-acc" : "tc-ban";
      line = won ? "line-acc" : "line-ban";
    } else {
      head = (info.winner == "team1" ? info.team1 : info.team2) + " won";
      tone = "tc-acc";
      line = "line-acc";
    }
    w.Text(slot, "at_e_head", head);
    w.Only(slot, "at_e_head", kTextTones, tone);
    w.Only(slot, "at_e_how", kTextTones, tone);
    w.Only(slot, "at_e_howbox", kTints, std::string(tone) == "tc-ban" ? "tint-ban" : std::string(tone) == "tc-acc" ? "tint-acc" : "tint-none");
    w.Only(slot, "at_ecard", kLines, line);
    for (int t = 0; t < 2; ++t) {
      const auto& v = t == 0 ? d.team1 : d.team2;
      for (int i = 0; i < 6; ++i) {
        const bool me = i < static_cast<int>(v.size()) && v[static_cast<size_t>(i)].id == hu.steamid64;
        const std::string r = std::string("at_e") + (t == 0 ? 'a' : 'b') + std::to_string(i);
        w.Only(slot, r, kTints, me ? "tint-acc" : "tint-none");
        w.Class(slot, r + "_n", "bold", me);
      }
    }
    // The strip: the viewer's team's rounds in the accent (spectators: the winner's).
    const int accentTeam = myTeam ? myTeam : (info.winner == "team2" ? 2 : 1);
    for (int i = 0; i < static_cast<int>(d.rounds.size()); ++i) {
      const EndScreenRound& x = d.rounds[static_cast<size_t>(i)];
      const std::string r = "at_rt" + std::to_string(i);
      const char* fill = x.winner == accentTeam ? "fill-acc" : "fill-muted";
      w.Only(slot, r + "_u", kFills, x.winner == 1 ? fill : "fill-none");
      w.Only(slot, r + "_d", kFills, x.winner == 2 ? fill : "fill-none");
    }
    w.Class(slot, "at_end", "on", true);  // per player too: a preview may have left its own value
    if (kHoldCursor && HasCapture(h) && h->set_capture(k, slot, 1)) g.captured.insert(slot);
  }
  w.Class(ALL, "at_end", "on", true);
  if (target != RU_HUD_ALL) {
    g.preview = true;
    g.active = true;  // a preview closes like the real one
    g.until = host::NowSeconds() + info.seconds;
    g.lastSecs = info.seconds;
    g.nextLabel = info.nextLabel;
    g.next = info.next;
  }
  Print("end-screen: shown for %ds (%zu + %zu rows, %zu rounds)%s\n", info.seconds, d.team1.size(), d.team2.size(),
        d.rounds.size(), target == RU_HUD_ALL ? "" : " (preview)");
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
  // Per player too: the show set it per player (a preview only for one).
  for (const auto& hu : ListHumans()) {
    if (hu.slot >= 0 && hu.slot <= 63) h->set_class(k, hu.slot, "at_end", "on", 0);
  }
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

EndScreenInfo EndScreenDemoInfo(uint64_t viewer, const std::string& viewerName, bool lost) {
  EndScreenInfo info;
  info.team1 = "Team A";
  info.team2 = "Team B";
  info.score1 = lost ? 9 : 13;
  info.score2 = lost ? 13 : 9;
  info.winner = lost ? "team2" : "team1";
  info.how = "By rounds";
  info.where = "MAP 2 OF 3 \xC2\xB7 INFERNO";
  info.nextLabel = "Next map:";
  info.next = "Mirage";
  info.seconds = 45;
  // K D A ADR KAST HS UD EK MVPs per player; the viewer is team 1's second row.
  const int a[5][9] = {{27, 14, 6, 112, 82, 54, 186, 6, 5}, {21, 15, 4, 91, 77, 41, 244, 3, 3}, {18, 16, 9, 84, 73, 38, 412, 2, 2},
                       {15, 17, 5, 71, 68, 47, 98, 4, 2}, {11, 18, 7, 58, 64, 29, 305, 1, 1}};
  const int b[5][9] = {{22, 18, 3, 96, 68, 45, 132, 5, 4}, {19, 19, 5, 83, 64, 52, 276, 3, 2}, {16, 19, 6, 74, 59, 36, 188, 2, 2},
                       {13, 20, 4, 66, 55, 31, 94, 1, 1}, {10, 21, 6, 52, 50, 28, 351, 0, 0}};
  const int rounds = 22;
  auto add = [&](int team, int i, const int* x) {
    stats::PlayerLine p;
    p.team = team;
    p.id = team == 1 && i == 1 ? viewer : 76561190000000000ull + static_cast<uint64_t>(team * 10 + i);
    p.name = team == 1 && i == 1 ? viewerName : "player " + std::to_string((team - 1) * 5 + i + 1);
    auto& s = p.stats;
    s.kills = x[0];
    s.deaths = x[1];
    s.assists = x[2];
    s.rounds_played = rounds;
    s.damage = x[3] * rounds;
    s.kast_rounds = (x[4] * rounds + 50) / 100;
    s.headshot_kills = (x[5] * x[0] + 50) / 100;
    s.utility_damage = x[6];
    s.entry_kills_t = x[7];
    s.mvp = x[8];
    info.stats.players.push_back(p);
  };
  for (int i = 0; i < 5; ++i) add(1, i, a[i]);
  for (int i = 0; i < 5; ++i) add(2, i, b[i]);
  // 13 - 9 for whoever won: the winner, then how (E elimination, B bomb, D defuse, T time).
  const char* strip = "aEbEaEaBaEbBaEaDbEaEaTbEbEaEbBbEaEbBaDbEaEaE";
  for (int i = 0; i < rounds; ++i) {
    stats::RoundSummary r;
    const bool a1 = (strip[i * 2] == 'a') != lost;
    r.winner_team = a1 ? 1 : 2;
    const char how = strip[i * 2 + 1];
    r.reason = how == 'B' ? 1 : how == 'D' ? 7 : how == 'T' ? 12 : 8;
    info.stats.rounds.push_back(r);
  }
  info.stats.team1.score_t = lost ? 5 : 8;
  info.stats.team1.score_ct = lost ? 4 : 5;
  info.stats.team2.score_t = lost ? 8 : 5;
  info.stats.team2.score_ct = lost ? 5 : 4;
  return info;
}

void EndScreenHidePreview() {
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g.active || !g.preview) return;
  }
  EndScreenHide();
}

bool EndScreenActive() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g.active;
}

}  // namespace readyup
