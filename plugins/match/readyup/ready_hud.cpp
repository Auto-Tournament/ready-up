#include "readyup/ready_hud.h"

#include "readyup/engine.h"
#include "readyup/config.h"
#include "readyup/logging.h"
#include "readyup/match_state.h"
#include "readyup/modes.h"
#include "readyup/player_registry.h"
#include "readyup/scrim_flow.h"
#include "readyup/players.h"
#include "readyup/webhook.h"
#include "readyup/welcome.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace readyup {
namespace {

using Clock = std::chrono::steady_clock;

constexpr int kMaxNamesPerSide = 6;
constexpr size_t kMaxNameBytes = 12;

// Palette (matches the welcome screen).
constexpr const char* kCt = "#5EA8FF";
constexpr const char* kT = "#FF9D3B";
constexpr const char* kGold = "#FFD23F";
constexpr const char* kGrey = "#8A8F98";
constexpr const char* kDim = "#5B6068";
constexpr const char* kOk = "#7CD35A";
constexpr const char* kNo = "#F87171";

struct Entry {
  uint64_t sid = 0;
  std::string name;
  int side = 0;  // 2 = T, 3 = CT
  bool ready = false;
  bool connected = true;
};

struct Board {
  std::string title;      // e.g. "scrim warmup"
  std::string countdown;  // e.g. "knife in 3" (empty when none)
  std::vector<Entry> entries;
  int botsCt = 0;
  int botsT = 0;
  int ready = 0;
  int total = 0;
  bool scrim = false;
  std::string note;  // e.g. "need players on both CT and T" (empty when none)
};

struct Sent {
  std::string html;
  Clock::time_point at{};
};

std::unordered_map<int, Sent> g_sent;  // GameFrame thread only
Clock::time_point g_lastRun{};
std::atomic<bool> g_showing{false};

// `.ru hudtest <n>` requests: steamid64 -> (variant, shown until).
constexpr auto kTestShowFor = std::chrono::seconds(10);
std::mutex g_testMu;
std::unordered_map<uint64_t, std::pair<int, Clock::time_point>> g_tests;

// Public test images for `.ru hudtest` (all checked to return 200 with curl).
constexpr const char* kTestSvg = "https://raw.githubusercontent.com/Auto-Tournament/auto-tournament/main/client/public/icon.svg";
constexpr const char* kTestPng = "https://raw.githubusercontent.com/Auto-Tournament/auto-tournament/main/client/public/icon-192.png";
constexpr const char* kTestPngWiki =
    "https://upload.wikimedia.org/wikipedia/commons/thumb/4/47/PNG_transparency_demonstration_1.png/"
    "250px-PNG_transparency_demonstration_1.png";

static std::string Esc(const std::string& in, size_t maxBytes) {
  std::string s = in;
  if (s.size() > maxBytes) {
    size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    s.resize(cut);
    s += "..";
  }
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '&': out += "&amp;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default:
        if (static_cast<unsigned char>(c) >= 0x20) out += c;
        break;
    }
  }
  return out;
}

static std::string Font(const char* color, const std::string& inner) {
  return std::string("<font color='") + color + "'>" + inner + "</font>";
}

// A URL that can sit inside a single-quoted HTML attribute (http/https only).
static std::string AttrSafeUrl(const std::string& url) {
  if (url.empty() || url.size() > 300) return {};
  if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) return {};
  for (unsigned char c : url) {
    if (c <= 0x20 || c == '\'' || c == '"' || c == '<' || c == '>' || c == '\\' || c >= 0x7F) return {};
  }
  return url;
}

static std::string Img(const std::string& url, int height) {
  return "<img src='" + url + "' width='" + std::to_string(height) + "' height='" + std::to_string(height) + "'>";
}

static std::string NameOf(uint64_t sid, const std::unordered_map<uint64_t, std::string>& names) {
  auto it = names.find(sid);
  if (it != names.end() && !it->second.empty()) return it->second;
  for (const auto& p : ListObservedPlayers()) {
    if (p.steamid64 == sid && !p.name.empty()) return p.name;
  }
  const std::string s = std::to_string(static_cast<unsigned long long>(sid));
  return "#" + s.substr(s.size() > 4 ? s.size() - 4 : 0);
}

static Board BuildBoard(ReadyUpMode mode, const std::optional<WebhookMatchContext>& ctx,
                        const std::vector<HumanIdentity>& humans) {
  Board b;
  std::unordered_map<uint64_t, std::string> names;
  for (const auto& h : humans) names[h.steamid64] = h.name;

  if (mode == ReadyUpMode::ScrimWarmup || !ctx) {
    b.scrim = true;
    b.title = "scrim warmup";
    const auto roster = BuildScrimRoster();
    for (const auto& kv : roster.teamNum) {
      Entry e;
      e.sid = kv.first;
      e.side = kv.second;
      e.name = NameOf(kv.first, names);
      e.ready = IsReady(kv.first);
      b.entries.push_back(std::move(e));
    }
    for (const auto& kv : roster.devBots) {
      if (kv.second == 3) b.botsCt++;
      else if (kv.second == 2) b.botsT++;
    }
    if (!CountScrimRoster(roster).bothSides) {
      b.note = DevBotsReadyEnabled() ? "need players (or bots) on both CT and T" : "need players on both CT and T";
    }
    const int cd = ScrimCountdownSecondsLeft();
    if (cd >= 0) b.countdown = std::string(Cfg().scrim_knife ? "knife in " : "live in ") + std::to_string(cd);
  } else {
    b.scrim = (ctx->slug == "scrim");
    b.title = RecoveryGateEnabled() ? "recovered match" : (b.scrim ? "scrim" : "match warmup");
    const auto ms = MatchStateGet();
    const int mapNum = ms.map_number <= 0 ? 1 : ms.map_number;
    bool team1Ct = true;
    if (mapNum >= 1 && static_cast<size_t>(mapNum) <= ctx->map_sides.size() &&
        ctx->map_sides[static_cast<size_t>(mapNum - 1)] == "team2_ct") {
      team1Ct = false;
    }
    std::unordered_set<uint64_t> connected;
    for (const auto& h : humans) connected.insert(h.steamid64);
    for (const auto& kv : ctx->roster_team) {
      if (kv.first == 0) continue;
      Entry e;
      e.sid = kv.first;
      const bool t1 = (kv.second == WebhookTeam::Team1);
      e.side = (t1 == team1Ct) ? 3 : 2;
      e.name = NameOf(kv.first, names);
      e.ready = IsReady(kv.first);
      e.connected = connected.count(kv.first) != 0;
      b.entries.push_back(std::move(e));
    }
    if (DevBotsReadyEnabled()) {
      for (const auto& bot : ListBots()) {
        if (bot.team == 3) b.botsCt++;
        else if (bot.team == 2) b.botsT++;
      }
    }
  }
  std::sort(b.entries.begin(), b.entries.end(),
            [](const Entry& x, const Entry& y) { return x.name < y.name; });
  for (const auto& e : b.entries) {
    b.total++;
    if (e.ready) b.ready++;
  }
  return b;
}

// One side's line. Entries are grouped by color (ready, not ready, not
// connected) so each run costs one <font> tag: keeps 5v5 well under ~1 KB.
static std::string SideLine(const Board& b, int side, uint64_t viewer) {
  std::string s = Font(side == 3 ? kCt : kT, side == 3 ? "<b>CT</b>" : "<b>T</b>");
  std::vector<const Entry*> list;
  for (const auto& e : b.entries) {
    if (e.side == side) list.push_back(&e);
  }
  auto rank = [](const Entry* e) { return !e->connected ? 2 : (e->ready ? 0 : 1); };
  std::stable_sort(list.begin(), list.end(), [&](const Entry* x, const Entry* y) { return rank(x) < rank(y); });

  int shown = 0, hidden = 0;
  const char* runColor = nullptr;
  std::string run;
  auto flush = [&]() {
    if (runColor && !run.empty()) s += "&#160; " + Font(runColor, run);
    run.clear();
  };
  for (const Entry* e : list) {
    // Keep the viewer visible even past the cap.
    if (shown >= kMaxNamesPerSide && e->sid != viewer) {
      hidden++;
      continue;
    }
    const char* col = !e->connected ? kDim : (e->ready ? kOk : kNo);
    if (col != runColor) {
      flush();
      runColor = col;
    } else {
      run += "&#160; ";
    }
    run += e->ready ? "&#10004;" : "&#10006;";
    const std::string nm = Esc(e->name, kMaxNameBytes);
    run += (e->sid == viewer) ? "<b>" + nm + "</b>" : nm;
    shown++;
  }
  flush();
  if (hidden > 0) s += " " + Font(kGrey, "+" + std::to_string(hidden));
  const int bots = (side == 3) ? b.botsCt : b.botsT;
  if (bots > 0) s += " " + Font(kOk, "&#10004;" + std::to_string(bots) + (bots == 1 ? " bot" : " bots"));
  if (shown == 0 && hidden == 0 && bots == 0) s += " " + Font(kDim, "-");
  return s;
}

static std::string ReadyHtml(const Board& b, uint64_t viewer, const std::string& footer) {
  std::string h;
  h.reserve(768);
  const std::string brand = HudBrandHtml(/*imgHeight=*/20, "");
  if (!brand.empty()) h += brand + " " + Font(kGrey, "&#183; " + b.title);
  else h += Font(kGrey, b.title);
  if (!b.countdown.empty()) h += " " + Font(kGold, "&#183; <b>" + b.countdown + "</b>");
  h += "<br>";
  h += Font(b.total > 0 && b.ready == b.total ? kOk : kGold,
            "<b>" + std::to_string(b.ready) + "/" + std::to_string(b.total) + " ready</b>");
  h += "<br>" + SideLine(b, 3, viewer) + "<br>" + SideLine(b, 2, viewer) + "<br>";

  const Entry* me = nullptr;
  for (const auto& e : b.entries) {
    if (e.sid == viewer) me = &e;
  }
  if (!me) {
    h += Font(kGrey, b.scrim ? "join CT or T to play" : "spectating");
  } else if (me->ready) {
    h += Font(kGrey, "you are ready &#183; <b>.ur</b> to cancel");
  } else {
    h += Font(kGold, "type <b>.r</b> in chat when ready");
  }
  if (!b.note.empty()) h += "<br>" + Font(kGrey, b.note);
  if (!footer.empty()) h += "<br>" + footer;
  return h;
}

static std::string KnifeHtml(const KnifeHudInfo& k, int viewerSide) {
  std::string h;
  h.reserve(384);
  h += Font(kGold, "<b>KNIFE ROUND</b>") + "<br>";
  if (k.phase == KnifePhase::Starting) {
    h += "knives only &#183; the winning team picks its side";
    return h;
  }
  const char* col = (k.winnerCs == 3) ? kCt : kT;
  h += Font(col, "<b>" + Esc(k.winnerName, 24) + "</b>") + " won";
  if (!k.reasonShort.empty()) h += " " + Font(kGrey, "(" + k.reasonShort + ")");
  h += "<br>";
  const std::string secs = std::to_string(k.secondsLeft) + "s";
  if (viewerSide == k.winnerCs) {
    h += Font(kOk, "type <b>.stay</b> or <b>.switch</b>") + "<br>";
    h += Font(kGrey, "no pick in " + secs + " = stay");
  } else {
    h += Font(kGrey, "waiting for " + Esc(k.winnerName, 24) + " to pick a side &#183; " + secs);
  }
  return h;
}

// `.ru hudtest <n>` variants. Each one fits well under ~1 KB.
static std::string TestHtml(int n) {
  const std::string title = Font(kGrey, "hudtest " + std::to_string(n)) + "<br>";
  switch (n) {
    case 1:
      return title +
             "<font class='fontSize-s'>fontSize-s</font> <font class='fontSize-sm'>sm</font> "
             "<font class='fontSize-m'>m</font><br>"
             "<font class='fontSize-l'>fontSize-l</font> <font class='fontSize-xl'>xl</font> "
             "<font class='fontSize-xxl'>xxl</font><br>"
             "<span class='fontWeight-Bold'>fontWeight-Bold</span> <b>b-tag</b> <i>i-tag</i> <u>u-tag</u><br>" +
             Font(kGold, "gold") + " " + Font(kCt, "CT") + " " + Font(kT, "T") + " " + Font(kOk, "ok") + " " +
             Font(kNo, "no") + " <span style='color:#C084FC'>span-style</span> <font color='red'>named-red</font>";
    case 2:
      return title + "A svg " + Img(kTestSvg, 32) + " | B png " + Img(kTestPng, 32) + "<br>C wiki png " +
             Img(kTestPngWiki, 40) + "<br>" + Font(kGrey, "missing = that format does not render");
    case 3:
      return title +
             "raw utf-8: \xE2\x9C\x94 \xE2\x9C\x96 \xE2\x98\x85 \xC2\xB7 \xE2\x80\xA2 \xE2\x86\x92<br>"
             "decimal: &#10004; &#10006; &#9733; &#183; &#8226; &#8594;<br>"
             "hex: &#x2714; &#x2716; &#x2605;<br>"
             "named: &check; &cross; &star; &middot; &bull; &rarr; &amp; &lt;b&gt;<br>"
             "emoji: \xF0\x9F\x8F\x86 \xF0\x9F\x94\xAA \xE2\x9C\x85 \xE2\x9D\x8C";
    case 4:
      return title + "svg (Auto Tournament icon.svg, height 64)<br>" + Img(kTestSvg, 64);
    case 5:
      return title + "png (Auto Tournament icon-192.png, height 64)<br>" + Img(kTestPng, 64);
    case 6:
      return title + "png (wikimedia 250px, height 64)<br>" + Img(kTestPngWiki, 64);
    case 7: {
      const auto c = Cfg();
      const std::string url = AttrSafeUrl(c.hud_logo_url);
      std::string h = title + HudBrandHtml(24, "fontSize-l") + "<br>";
      h += Font(kGrey, "hud_logo_url=" + (url.empty() ? std::string("(none)") : Esc(url, 80)));
      if (!c.hud_logo_url.empty() && url.empty()) h += "<br>" + Font(kNo, "hud_logo_url rejected (needs http(s), no quotes/spaces)");
      return h;
    }
    default:
      return {};
  }
}

static const char* TestDescription(int n) {
  switch (n) {
    case 1: return "font classes (fontSize-s..xxl, fontWeight-Bold), b/i/u, colors";
    case 2: return "images: A = Auto Tournament SVG, B = Auto Tournament PNG, C = wikimedia PNG";
    case 3: return "unicode: raw UTF-8, decimal/hex/named entities, emoji";
    case 4: return "SVG only (Auto Tournament icon.svg)";
    case 5: return "PNG only (Auto Tournament icon-192.png)";
    case 6: return "PNG only (wikimedia 250px)";
    case 7: return "configured header (hud_logo_url + hud_brand)";
    default: return nullptr;
  }
}

static bool HudEnabled() {
  return Cfg().ready_hud;
}

// Last per-client send result; assume it works until a send fails.
std::atomic<bool> g_sendOk{true};

}  // namespace

bool HudReplacesChat() { return HudEnabled() && g_sendOk.load(std::memory_order_relaxed); }

std::string HudBrandHtml(int imgHeight, const char* fontClass) {
  if (!FeatureEnabled(Feature::HudBrand)) return {};
  const auto c = Cfg();
  std::string h;
  const std::string url = AttrSafeUrl(c.hud_logo_url);
  if (!url.empty()) h += Img(url, std::max(8, std::min(128, imgHeight)));
  if (!c.hud_brand.empty()) {
    if (!h.empty()) h += " ";
    h += "<font ";
    if (fontClass && *fontClass) h += std::string("class='") + fontClass + "' ";
    h += std::string("color='") + kGold + "'><b>" + Esc(c.hud_brand, 40) + "</b></font>";
  }
  return h;
}

bool ReadyHudShowing() {
  return g_showing.load(std::memory_order_relaxed) && HudEnabled();
}

std::string ReadyHudRequestTest(uint64_t steamid64, int variant) {
  const char* desc = TestDescription(variant);
  if (!desc || steamid64 == 0) return {};
  std::lock_guard<std::mutex> lk(g_testMu);
  g_tests[steamid64] = {variant, Clock::now() + kTestShowFor};
  return desc;
}

void ReadyHudTick() {
  if (IsDisabled()) return;
  const auto now = Clock::now();
  if (g_lastRun.time_since_epoch().count() != 0 && (now - g_lastRun) < std::chrono::milliseconds(Cfg().hud_tick_ms)) return;
  g_lastRun = now;

  // Pending `.ru hudtest` panels (shown even with ready_hud=0 and outside warmup).
  std::unordered_map<uint64_t, int> tests;
  {
    std::lock_guard<std::mutex> lk(g_testMu);
    for (auto it = g_tests.begin(); it != g_tests.end();) {
      if (now >= it->second.second) {
        it = g_tests.erase(it);
      } else {
        tests[it->first] = it->second.first;
        ++it;
      }
    }
  }

  const bool enabled = HudEnabled();
  if (!enabled) g_showing.store(false, std::memory_order_relaxed);
  if (!enabled && tests.empty()) {
    g_sent.clear();
    return;
  }

  const ReadyUpMode mode = GetMode();
  const auto ctx = WebhookGetMatchContext();

  enum class What { None, Ready, Knife };
  What what = What::None;
  KnifeHudInfo knife;
  if (!enabled) {
    what = What::None;
  } else if (mode == ReadyUpMode::ScrimWarmup) {
    what = What::Ready;
  } else if (mode == ReadyUpMode::MatchWarmup && ctx && WarmupEnabled() && !GoLiveTriggered()) {
    what = What::Ready;
  } else if (mode == ReadyUpMode::MatchKnife) {
    knife = KnifeHudSnapshot();
    // knife.cfg has almost no freeze time, so "Starting" lasts about a second.
    // Keep the KNIFE ROUND panel up for hud_knife_hold_s after the knife round
    // begins, also while it is running.
    static Clock::time_point s_knifeSince{};
    if (knife.phase == KnifePhase::None) {
      s_knifeSince = {};
    } else if (s_knifeSince == Clock::time_point{}) {
      s_knifeSince = Clock::now();
    }
    const bool holding = knife.phase == KnifePhase::Running && s_knifeSince != Clock::time_point{} &&
                         (Clock::now() - s_knifeSince) < std::chrono::seconds(Cfg().hud_knife_hold_s);
    if (knife.phase == KnifePhase::Starting || knife.phase == KnifePhase::Picking || holding) {
      what = What::Knife;
      if (holding) knife.phase = KnifePhase::Starting;  // same "knives only" text while held
    }
  }
  if (what == What::None && tests.empty()) {
    g_sent.clear();
    return;
  }

  const auto humans = ListHumans();
  Board board;
  std::string footer;
  if (what == What::Ready) {
    board = BuildBoard(mode, ctx, humans);
    footer = WarmupHtmlCustom();
    if (footer.size() > 256) footer.clear();  // keep the panel small
  }

  std::unordered_set<int> present;
  static std::atomic<bool> s_warned{false};
  static std::atomic<bool> s_loggedSize{false};
  for (const auto& h : humans) {
    // Engine slot from events when known; else the log `<N>` (the player slot in CS2).
    const int slot = h.slot >= 0 ? h.slot : h.userid;
    if (h.steamid64 == 0 || slot < 0 || slot > 63) continue;
    present.insert(slot);

    std::string html;
    const auto t = tests.find(h.steamid64);
    if (t != tests.end()) {
      html = TestHtml(t->second);  // over the HUD and the welcome card
    } else {
      if (what == What::None) continue;
      // The welcome screen owns the panel while it is up (~5s after joining).
      if (WelcomeActiveForSteam(h.steamid64)) continue;
      html = (what == What::Ready) ? ReadyHtml(board, h.steamid64, footer) : KnifeHtml(knife, h.team);
    }
    if (html.empty()) continue;

    // Same HTML: resend once a second (2s duration) so the panel never lapses
    // and its fade-in does not restart. Changed HTML: send right away.
    auto& s = g_sent[slot];
    const bool changed = (s.html != html);
    if (s.html == html && (now - s.at) < std::chrono::milliseconds(Cfg().hud_resend_ms)) continue;
    const bool ok = PrintCenterHtmlToClientOnly(slot, html, Cfg().hud_duration_s);
    g_sendOk.store(ok, std::memory_order_relaxed);
    if (ok) {
      s.html = html;
      s.at = now;
      if (t == tests.end()) g_showing.store(true, std::memory_order_relaxed);
      if (t != tests.end() && changed) {
        Debug("ready-hud: hudtest %d -> slot=%d (%zu bytes html)\n", t->second, slot, html.size());
      }
      if (!s_loggedSize.exchange(true)) Debug("ready-hud: first panel sent (%zu bytes html)\n", html.size());
      static std::atomic<bool> s_warnedBig{false};
      if (html.size() > 1000 && !s_warnedBig.exchange(true)) {
        Print("ready-hud: panel is %zu bytes (large; the client may cut it off)\n", html.size());
      }
    } else {
      g_showing.store(false, std::memory_order_relaxed);
      if (!s_warned.exchange(true)) PrintLine("ready-hud: per-client center HTML unavailable; ready HUD not shown.");
    }
  }
  for (auto it = g_sent.begin(); it != g_sent.end();) {
    if (present.count(it->first) == 0) it = g_sent.erase(it);
    else ++it;
  }
}

}  // namespace readyup
