// Offline tests for the go-live card and `.admin` (readyup/card_html.h, readyup/admin_call_logic.h):
// the card HTML (escaping, size, which commands show), the admin card, the per-player cooldown,
// the message cleanup, the call id / timestamp and the `admin_called` payload.
// ctest `match_live_cards`.
#include "readyup/admin_call_logic.h"
#include "readyup/card_html.h"

#include <cstdio>
#include <string>

using namespace readyup;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    ++g_checks;                                                                     \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

static bool Has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

static void TestEscape() {
  CHECK(CardHtmlEscape("a<b>&\"'c", 100) == "a&lt;b&gt;&amp;&quot;&#39;c");
  CHECK(CardHtmlEscape("\x07red\x01 name", 100) == "red name");  // chat color bytes dropped
  CHECK(CardHtmlEscape("abcdef", 3) == "abc...");
  // "é" is two bytes: a cut inside it backs off to the character start.
  CHECK(CardHtmlEscape("ab\xC3\xA9z", 3) == "ab...");
}

static void TestGoLiveCard() {
  GoLiveCardInfo m;
  m.team1 = "Team <A>";
  m.team2 = "B & Co";
  m.team1Side = 2;  // team1 on T
  m.pauses = true;
  const std::string h = GoLiveCardHtml(m);
  CHECK(Has(h, "<b>LIVE</b>") && Has(h, "GO GO GO"));
  CHECK(Has(h, "Team &lt;A&gt; (T)") && Has(h, "B &amp; Co (CT)"));
  CHECK(!Has(h, "<A>"));
  CHECK(Has(h, "#FF9D3B'>Team &lt;A&gt; (T)"));  // T orange
  CHECK(Has(h, "#5EA8FF'>B &amp; Co (CT)"));     // CT blue
  for (const char* cmd : {".p<", ".pause<", ".tech<", ".up<", ".unpause<", ".tac<", ".admin<"}) CHECK(Has(h, cmd));
  CHECK(h.size() < 1024);

  // Long names are cut: the card stays small.
  GoLiveCardInfo big = m;
  big.team1 = std::string(200, 'x');
  big.team2 = std::string(200, '&');
  CHECK(GoLiveCardHtml(big).size() < 1024);

  // Scrim / no names: no matchup line; no match: only .admin.
  GoLiveCardInfo s;
  s.pauses = true;
  const std::string hs = GoLiveCardHtml(s);
  CHECK(!Has(hs, " vs ") && Has(hs, ".pause<") && Has(hs, ".admin<"));
  GoLiveCardInfo none;
  const std::string hn = GoLiveCardHtml(none);
  CHECK(!Has(hn, ".pause<") && !Has(hn, ".tac<") && Has(hn, ".admin<"));

  // Unknown side: names without (CT)/(T).
  GoLiveCardInfo u;
  u.team1 = "A";
  u.team2 = "B";
  const std::string hu = GoLiveCardHtml(u);
  CHECK(Has(hu, ">A</font>") && Has(hu, ">B</font>") && !Has(hu, "(CT)"));
}

static void TestAdminCard() {
  const std::string h = AdminCallCardHtml("<b>evil</b>", "Team A, CT", "bot is <stuck> & lagging");
  CHECK(Has(h, "ADMIN CALLED"));
  CHECK(Has(h, "&lt;b&gt;evil&lt;/b&gt; (Team A, CT) needs an admin"));
  CHECK(Has(h, "bot is &lt;stuck&gt; &amp; lagging"));
  CHECK(!Has(AdminCallCardHtml("n", "", ""), "()"));
  CHECK(AdminCallCardHtml(std::string(300, 'n'), std::string(300, 't'), std::string(300, '<')).size() < 1024);
}

static void TestCooldown() {
  AdminCallCooldown c;
  int left = -1;
  CHECK(c.TryCall(1, 100.0, 60, &left) && left == 0);
  CHECK(!c.TryCall(1, 100.5, 60, &left) && left == 60);
  CHECK(!c.TryCall(1, 159.2, 60, &left) && left == 1);
  CHECK(c.TryCall(2, 101.0, 60, &left));  // per player
  CHECK(c.TryCall(1, 160.0, 60, &left));  // over
  CHECK(!c.TryCall(1, 161.0, 60, &left) && left == 59);
  // 0 = no cooldown.
  CHECK(c.TryCall(3, 5.0, 0) && c.TryCall(3, 5.0, 0));
  c.Clear();
  CHECK(c.TryCall(1, 161.0, 60));
}

static void TestMessage() {
  CHECK(CleanAdminCallMessage("") == "");
  CHECK(CleanAdminCallMessage("   ") == "");
  CHECK(CleanAdminCallMessage("  need   help\tnow \n") == "need help now");
  CHECK(CleanAdminCallMessage("\x07red\x01text") == "redtext");
  CHECK(CleanAdminCallMessage(std::string(500, 'a')).size() == 200);
  // 200 characters, not bytes: "é" x 250 keeps 200 of them (400 bytes), never half of one.
  std::string e;
  for (int i = 0; i < 250; ++i) e += "\xC3\xA9";
  CHECK(CleanAdminCallMessage(e).size() == 400);
  CHECK(CleanAdminCallMessage("abc def", 4) == "abc");  // no trailing space after the cut
}

static void TestIdsAndTime() {
  CHECK(IsoUtcFromUnixMs(0) == "1970-01-01T00:00:00.000Z");
  CHECK(IsoUtcFromUnixMs(1790340012345LL) == "2026-09-25T12:40:12.345Z");
  unsigned char b[16];
  for (int i = 0; i < 16; ++i) b[i] = static_cast<unsigned char>(0xF0 + i);
  const std::string id = CallIdFromBytes(b);
  CHECK(id.size() == 36 && id[8] == '-' && id[13] == '-' && id[14] == '4' && id[18] == '-' && id[23] == '-');
  CHECK(id[19] == '8' || id[19] == '9' || id[19] == 'a' || id[19] == 'b');
  CHECK(id == "f0f1f2f3-f4f5-46f7-b8f9-fafbfcfdfeff");
}

static void TestPayload() {
  AdminCallEvent e;
  e.hasMatch = true;
  e.matchid = 4242;
  e.map_number = 2;
  e.call_id = "f0f1f2f3-f4f5-46f7-b8f9-fafbfcfdfeff";
  e.player.steamid64 = 76561198000000001ULL;
  e.player.name = "ali \"ce\"";
  e.player.team = "team1";
  e.player.side = "ct";
  e.message = "smoke bug on B";
  e.called_at = "2026-09-25T11:20:12.345Z";
  const std::string j = AdminCalledWebhookJson(e);
  CHECK(j ==
        "{\"event\":\"admin_called\",\"matchid\":4242,\"map_number\":2,"
        "\"call_id\":\"f0f1f2f3-f4f5-46f7-b8f9-fafbfcfdfeff\","
        "\"player\":{\"steamid64\":\"76561198000000001\",\"name\":\"ali \\\"ce\\\"\",\"team\":\"team1\",\"side\":\"ct\"},"
        "\"message\":\"smoke bug on B\",\"called_at\":\"2026-09-25T11:20:12.345Z\"}");

  // Scrim / no match: matchid null; unknown team / side null; empty message allowed.
  AdminCallEvent s = e;
  s.hasMatch = false;
  s.player.team = "";
  s.player.side = "";
  s.message = "";
  const std::string js = AdminCalledWebhookJson(s);
  CHECK(Has(js, "\"matchid\":null,") && Has(js, "\"team\":null,\"side\":null") && Has(js, "\"message\":\"\""));

  // The fleet data: the same fields without event / matchid / map_number.
  const std::string d = AdminCallData(e).Dump();
  CHECK(d.rfind("{\"call_id\":", 0) == 0 && !Has(d, "\"event\"") && !Has(d, "matchid"));

  CHECK(AdminCallTeamLabel("Team A", "ct", false) == "Team A, CT");
  CHECK(AdminCallTeamLabel("", "t", false) == "T");
  CHECK(AdminCallTeamLabel("Team A", "", false) == "Team A");
  CHECK(AdminCallTeamLabel("Team A", "ct", true) == "spectator");
  CHECK(AdminCallTeamLabel("", "", false).empty());
}

int main() {
  TestEscape();
  TestGoLiveCard();
  TestAdminCard();
  TestCooldown();
  TestMessage();
  TestIdsAndTime();
  TestPayload();
  if (g_failures) {
    std::fprintf(stderr, "live_cards_test: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("live_cards_test: all %d checks passed\n", g_checks);
  return 0;
}
