// Offline tests for core/src/readyup/chat_highlight.h: commands green, numbers plain, lines that
// carry color bytes untouched, the "Ready Up: " lead-in dropped. ctest `chat_highlight`.
#include "readyup/chat_highlight.h"

#include <cstdio>
#include <string>

using namespace readyup;

static int g_failures = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

static const std::string G = "\x04", D = "\x01";

int main() {
  CHECK(HighlightChatText("Type .ready to start") == "Type " + G + ".ready" + D + " to start");
  CHECK(HighlightChatText("They forfeit in 240s.") == "They forfeit in 240s.");
  CHECK(HighlightChatText("13-9 after the overtimes") == "13-9 after the overtimes");
  CHECK(HighlightChatText("Map de_dust2 in a 2v2, MR12") == "Map de_dust2 in a 2v2, MR12");
  CHECK(HighlightChatText("Ready Up: game restarting.") == "Ready Up: game restarting.");
  const std::string colored = "\x07red already";
  CHECK(HighlightChatText(colored) == colored);
  CHECK(HighlightChatText("") == "");
  CHECK(WithoutReadyUpLeadIn("Ready Up: LIVE! Good luck, have fun.") == "LIVE! Good luck, have fun.");
  CHECK(WithoutReadyUpLeadIn("Pause: .p") == "Pause: .p");
  if (g_failures) std::fprintf(stderr, "chat_highlight_test: %d failure(s)\n", g_failures);
  else std::printf("chat_highlight_test: ok\n");
  return g_failures ? 1 : 0;
}
