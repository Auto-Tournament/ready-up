// core/src/readyup/player_language.h: codes, CS2 names, player_languages.json. ctest `player_language`.
#include "readyup/player_language.h"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

#define CHECK_EQ(a, b)                                                                              \
  do {                                                                                              \
    const std::string x_ = (a), y_ = (b);                                                           \
    if (x_ != y_) {                                                                                 \
      std::fprintf(stderr, "%s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, x_.c_str(), y_.c_str()); \
      ++g_failures;                                                                                 \
    }                                                                                               \
  } while (0)

using namespace readyup::lang;

std::string CodeOf(const std::string& s) {
  const Language* l = Find(s);
  return l ? l->code : "(none)";
}

}  // namespace

int main() {
  size_t n = 0;
  Languages(&n);
  CHECK_EQ(std::to_string(n), "29");  // the platform's languages

  CHECK_EQ(CodeOf("de"), "de");
  CHECK_EQ(CodeOf("DE"), "de");
  CHECK_EQ(CodeOf("pt-BR"), "pt-BR");
  CHECK_EQ(CodeOf("pt_br"), "pt-BR");
  CHECK_EQ(CodeOf("brazilian"), "pt-BR");
  CHECK_EQ(CodeOf("schinese"), "zh-CN");
  CHECK_EQ(CodeOf("tchinese"), "zh-TW");
  CHECK_EQ(CodeOf("latam"), "es-419");
  CHECK_EQ(CodeOf("koreana"), "ko");
  CHECK_EQ(CodeOf("norwegian"), "nb");
  CHECK_EQ(CodeOf("no"), "nb");
  CHECK_EQ(CodeOf("zh"), "zh-CN");
  CHECK_EQ(CodeOf("klingon"), "(none)");
  CHECK_EQ(CodeOf(""), "(none)");

  const auto choices = ParseChoices(R"({"version": 1, "players": {
      "76561198000000001": "de", "76561198000000002": "brazilian", "76561198000000003": "xx", "bad": "fr"}})");
  CHECK_EQ(std::to_string(choices.size()), "2");
  CHECK_EQ(choices.at(76561198000000002ull), "pt-BR");
  CHECK_EQ(std::to_string(ParseChoices(ChoicesJson(choices)) == choices), "1");
  CHECK_EQ(std::to_string(ParseChoices("not json").size()), "0");
  CHECK_EQ(ChoicesJson({}), "{\n  \"version\": 1,\n  \"players\": {}\n}\n");

  if (g_failures) {
    std::fprintf(stderr, "player_language: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("player_language: ok\n");
  return 0;
}
