// Offline tests for player chat to the platform (readyup/chat_forward.h) and the team lock's move
// rule (readyup/team_lock.h). ctest `match_chat_forward`.
#include "readyup/chat_forward.h"
#include "readyup/team_lock.h"

#include <cstdio>
#include <string>

using namespace readyup;
using namespace readyup::chat_forward;

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

static void TestTeamLock() {
  // 2026-10-17: after the map load the cached team is 0, the player is on CT and stays there.
  CHECK(!TeamMoveNeeded(0, 3, 3));
  CHECK(!TeamMoveNeeded(0, 2, 2));
  CHECK(!TeamMoveNeeded(0, 1, 1));
  // On the wrong side, whatever the cache says.
  CHECK(TeamMoveNeeded(3, 2, 3));
  CHECK(TeamMoveNeeded(0, 0, 3));
  // Engine unreadable (off the game thread, no controller): the cache decides, as before.
  CHECK(!TeamMoveNeeded(3, std::nullopt, 3));
  CHECK(TeamMoveNeeded(0, std::nullopt, 3));
  CHECK(TeamMoveNeeded(2, std::nullopt, 3));

  // Engine unreadable and the move does not take: 3 tries, then nothing until the team changes.
  MoveRetryCap cap(3);
  CHECK(cap.Allow(1, 3, 0) && cap.Allow(1, 3, 0) && cap.Allow(1, 3, 0));
  CHECK(!cap.Allow(1, 3, 0));
  CHECK(!cap.Allow(1, 3, 0));
  CHECK(cap.Allow(2, 3, 0));  // per player
  CHECK(cap.Allow(1, 3, 2));  // a team event arrived (cached team 2): tries again
  CHECK(cap.Allow(1, 2, 2));  // a new target (side swap) too
  cap.Forget(1);
  CHECK(cap.Allow(1, 2, 2));
}

static void TestParse() {
  auto c = ParseChatLogLine("L 10/17/2026 - 12:00:00: \"BBro<3><[U:1:12345]><CT>\" say \"nice one\"");
  CHECK(c.has_value());
  if (c) {
    CHECK(c->steamid64 == 76561197960278073ull);
    CHECK(c->name == "BBro");
    CHECK(c->side == "ct");
    CHECK(!c->team_only);
    CHECK(c->message == "nice one");
  }
  c = ParseChatLogLine("\"Emil YO<4><[U:1:2]><TERRORIST>\" say_team \"rush \"B\" now\"\n");
  CHECK(c.has_value());
  if (c) {
    CHECK(c->name == "Emil YO");
    CHECK(c->side == "t");
    CHECK(c->team_only);
    CHECK(c->message == "rush \"B\" now");
  }
  c = ParseChatLogLine("\"spec<5><[U:1:3]><Spectator>\" say \"hi\"");
  CHECK(c && c->side == "spec");
  c = ParseChatLogLine("\"new<6><[U:1:4]><Unassigned>\" say \"\"");
  CHECK(c && c->side == "spec" && c->message.empty());
  // Bots, the console and other lines.
  CHECK(!ParseChatLogLine("\"Rex<0><BOT><CT>\" say \"hello\""));
  CHECK(!ParseChatLogLine("\"Console<0><Console><Console>\" say \"server says\""));
  CHECK(!ParseChatLogLine("\"BBro<3><[U:1:12345]><CT>\" killed \"x<4><[U:1:2]><TERRORIST>\" with \"ak47\""));
  CHECK(!ParseChatLogLine("World triggered \"Round_Start\""));
  CHECK(!ParseChatLogLine("\"bad<3><><CT>\" say \"x\""));
}

static void TestCommands() {
  CHECK(IsCommand(".ready"));
  CHECK(IsCommand("!admin help"));
  CHECK(IsCommand("  .r"));
  CHECK(!IsCommand("."));
  CHECK(!IsCommand("gg."));
  CHECK(!IsCommand(""));
  CHECK(!IsCommand("ok"));
}

static void TestRateLimit() {
  RateLimiter r(5, 5.0);
  for (int i = 0; i < 5; ++i) CHECK(r.Allow(1, 10.0 + i * 0.1));
  CHECK(!r.Allow(1, 10.6));
  CHECK(r.Allow(2, 10.6));  // per player
  CHECK(!r.Allow(1, 14.9));
  CHECK(r.Allow(1, 15.0));  // the first line left the window
  CHECK(!r.Allow(1, 15.05));
  CHECK(r.Allow(1, 15.1));
}

static void TestEventData() {
  ChatLine c;
  c.steamid64 = 76561198000000001ull;
  c.name = "alice";
  c.side = "ct";
  c.team_only = true;
  c.message = std::string("  hi\x07 there  ");
  status::Json d = EventData(c, "team1", 1790340012345);
  CHECK(d.Dump() ==
        "{\"player\":{\"steamid64\":\"76561198000000001\",\"name\":\"alice\",\"team\":\"team1\",\"side\":\"ct\"},"
        "\"message\":\"hi there\",\"team_only\":true,\"at\":1790340012345}");
  d = EventData(c, "none", 1);
  CHECK(d.Find("player")->Find("team")->IsNull());
  c.message = std::string(400, 'x');
  d = EventData(c, "", 1);
  CHECK(d.Find("message")->AsString().size() == kMessageMaxChars);
}

int main() {
  TestTeamLock();
  TestParse();
  TestCommands();
  TestRateLimit();
  TestEventData();
  std::printf("chat_forward: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
