// libs/readyup/i18n: lookup, fallback, placeholders, plurals, lint. ctest `i18n`.
#include "readyup/i18n.h"

#include <cstdio>
#include <string>

extern const readyup::i18n::Locale kReadyUpLocales[];
extern const size_t kReadyUpLocaleCount;

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

using readyup::i18n::Catalog;
using readyup::i18n::Locale;
using readyup::i18n::PluralCategory;

const Locale kLocales[] = {
    {"en", R"({"hi": "Hello {name}", "only_en": "English only",
              "players_one": "{count} player", "players_other": "{count} players"})"},
    {"de", R"({"hi": "Hallo {name}", "players_one": "{count} Spieler", "players_other": "{count} Spieler"})"},
    {"es", R"({"hi": "Hola {name}"})"},
    {"ru", R"({"players_one": "{count} игрок", "players_few": "{count} игрока", "players_many": "{count} игроков"})"},
    {"pt-BR", R"({"hi": "Olá {name}"})"},
};

void TestLookup() {
  Catalog c;
  CHECK_EQ(c.Load(kLocales, sizeof(kLocales) / sizeof(kLocales[0])), "");
  CHECK_EQ(c.Tr("de", "hi", {{"name", "Kim"}}), "Hallo Kim");
  CHECK_EQ(c.Tr("es-419", "hi", {{"name", "Kim"}}), "Hola Kim");    // base language
  CHECK_EQ(c.Tr("pt-BR", "hi", {{"name", "Kim"}}), "Olá Kim");
  CHECK_EQ(c.Tr("pt-PT", "hi", {{"name", "Kim"}}), "Hello Kim");    // no pt, no pt-PT: en
  CHECK_EQ(c.Tr("de", "only_en"), "English only");                  // missing key: en
  CHECK_EQ(c.Tr("xx", "hi", {{"name", "Kim"}}), "Hello Kim");       // unknown language: en
  CHECK_EQ(c.Tr("de", "nope"), "nope");                             // unknown key: the key
  CHECK_EQ(c.Tr("en", "hi"), "Hello {name}");                       // missing arg stays
}

void TestPlurals() {
  Catalog c;
  c.Load(kLocales, sizeof(kLocales) / sizeof(kLocales[0]));
  CHECK_EQ(c.Tr("en", "players", {{"count", "1"}}), "1 player");
  CHECK_EQ(c.Tr("en", "players", {{"count", "0"}}), "0 players");
  CHECK_EQ(c.Tr("ru", "players", {{"count", "1"}}), "1 игрок");
  CHECK_EQ(c.Tr("ru", "players", {{"count", "21"}}), "21 игрок");
  CHECK_EQ(c.Tr("ru", "players", {{"count", "3"}}), "3 игрока");
  CHECK_EQ(c.Tr("ru", "players", {{"count", "12"}}), "12 игроков");
  CHECK_EQ(c.Tr("ru", "players", {{"count", "25"}}), "25 игроков");
  CHECK_EQ(c.Tr("ja", "players", {{"count", "1"}}), "1 player");  // no ja table: en, by en's rule
}

void TestPluralRules() {
  CHECK_EQ(PluralCategory("en", 1), "one");
  CHECK_EQ(PluralCategory("en", 0), "other");
  CHECK_EQ(PluralCategory("fr", 0), "one");
  CHECK_EQ(PluralCategory("pt-BR", 1), "one");
  CHECK_EQ(PluralCategory("pt-BR", 0), "one");
  CHECK_EQ(PluralCategory("pt-PT", 0), "other");
  CHECK_EQ(PluralCategory("pl", 1), "one");
  CHECK_EQ(PluralCategory("pl", 22), "few");
  CHECK_EQ(PluralCategory("pl", 21), "many");
  CHECK_EQ(PluralCategory("pl", 13), "many");
  CHECK_EQ(PluralCategory("uk", 101), "one");
  CHECK_EQ(PluralCategory("uk", 111), "many");
  CHECK_EQ(PluralCategory("cs", 4), "few");
  CHECK_EQ(PluralCategory("cs", 5), "other");
  CHECK_EQ(PluralCategory("ro", 0), "few");
  CHECK_EQ(PluralCategory("ro", 19), "few");
  CHECK_EQ(PluralCategory("ro", 20), "other");
  CHECK_EQ(PluralCategory("ro", 102), "few");
  CHECK_EQ(PluralCategory("ro", 101), "other");
  CHECK_EQ(PluralCategory("zh-CN", 1), "other");
  CHECK_EQ(PluralCategory("th", 1), "other");
  CHECK_EQ(PluralCategory("nb", 1), "one");
}

void TestLint() {
  const Locale ok[] = {
      {"en", R"({"a": "x {n}", "p_one": "one thing", "p_other": "{count} things"})"},
      {"ru", R"({"a": "y {n}", "p_one": "{count} вещь", "p_few": "{count} вещи", "p_many": "{count} вещей"})"},
  };
  Catalog good;
  good.Load(ok, 2);
  CHECK_EQ(std::to_string(readyup::i18n::Lint(good).size()), "0");

  const Locale bad[] = {
      {"en", R"({"a": "x {n}"})"},
      {"de", R"({"a": "y {m}", "b": "extra"})"},
  };
  Catalog c;
  c.Load(bad, 2);
  CHECK_EQ(std::to_string(readyup::i18n::Lint(c).size()), "2");

  Catalog broken;
  const Locale nojson[] = {{"en", "{"}};
  CHECK_EQ(std::to_string(broken.Load(nojson, 1).empty()), "0");
}

// tests/fixtures/locales, compiled in by readyup_embed_locales (UTF-8 survives the embedding).
void TestEmbedded() {
  Catalog c;
  CHECK_EQ(c.Load(kReadyUpLocales, kReadyUpLocaleCount), "");
  CHECK_EQ(c.Tr("ru", "greeting", {{"name", "Kim"}}), "Привет, Kim");
  CHECK_EQ(c.Tr("ru", "rounds", {{"count", "5"}}), "5 раундов");
  CHECK_EQ(c.Tr("de", "rounds", {{"count", "1"}}), "1 round");
}

}  // namespace

int main() {
  TestLookup();
  TestPlurals();
  TestPluralRules();
  TestLint();
  TestEmbedded();
  if (g_failures) {
    std::fprintf(stderr, "i18n: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("i18n: ok\n");
  return 0;
}
