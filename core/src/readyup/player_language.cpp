// The pure half of player_language.h (ctest `player_language`); the store is player_language_store.cpp.
#include "readyup/player_language.h"

#include "readyup/minijson.h"

#include <cctype>
#include <cstdlib>

namespace readyup::lang {
namespace {

// The platform's languages (client/src/locales) and CS2's names for them.
const Language kLanguages[] = {
    {"en", "english", "English"},
    {"zh-CN", "schinese", "简体中文"},
    {"zh-TW", "tchinese", "繁體中文"},
    {"fr", "french", "Français"},
    {"de", "german", "Deutsch"},
    {"es", "spanish", "Español (España)"},
    {"es-419", "latam", "Español (Latinoamérica)"},
    {"it", "italian", "Italiano"},
    {"pt-PT", "portuguese", "Português (Portugal)"},
    {"pt-BR", "brazilian", "Português (Brasil)"},
    {"pl", "polish", "Polski"},
    {"nl", "dutch", "Nederlands"},
    {"nb", "norwegian", "Norsk"},
    {"ru", "russian", "Русский"},
    {"uk", "ukrainian", "Українська"},
    {"tr", "turkish", "Türkçe"},
    {"ja", "japanese", "日本語"},
    {"ko", "koreana", "한국어"},
    {"cs", "czech", "Čeština"},
    {"sv", "swedish", "Svenska"},
    {"da", "danish", "Dansk"},
    {"fi", "finnish", "Suomi"},
    {"hu", "hungarian", "Magyar"},
    {"ro", "romanian", "Română"},
    {"bg", "bulgarian", "Български"},
    {"el", "greek", "Ελληνικά"},
    {"th", "thai", "ไทย"},
    {"vi", "vietnamese", "Tiếng Việt"},
    {"id", "indonesian", "Bahasa Indonesia"},
};

// Short forms people type that are not a code or a CS2 name.
const char* const kAliases[][2] = {{"zh", "zh-CN"}, {"pt", "pt-PT"}, {"no", "nb"}, {"nn", "nb"},
                                   {"korean", "ko"}, {"chinese", "zh-CN"}};

std::string Lower(std::string s) {
  for (char& c : s) c = c == '_' ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

const Language* Languages(size_t* count) {
  if (count) *count = sizeof(kLanguages) / sizeof(kLanguages[0]);
  return kLanguages;
}

const Language* Find(const std::string& codeOrName) {
  std::string s = Lower(codeOrName);
  for (const auto& a : kAliases) {
    if (s == a[0]) s = Lower(a[1]);
  }
  for (const Language& l : kLanguages) {
    if (s == Lower(l.code) || s == l.cs2) return &l;
  }
  return nullptr;
}

std::map<uint64_t, std::string> ParseChoices(const std::string& json) {
  std::map<uint64_t, std::string> out;
  minijson::ParseError err;
  const auto v = minijson::Parse(json, &err);
  const minijson::Value* players = v ? v->get("players") : nullptr;
  if (!minijson::IsObject(players)) return out;
  for (const auto& kv : players->obj) {
    const uint64_t id = std::strtoull(kv.first.c_str(), nullptr, 10);
    const Language* l = minijson::IsString(&kv.second) ? Find(kv.second.str) : nullptr;
    if (id && l) out[id] = l->code;
  }
  return out;
}

std::string ChoicesJson(const std::map<uint64_t, std::string>& choices) {
  std::string s = "{\n  \"version\": 1,\n  \"players\": {";
  bool first = true;
  for (const auto& [id, code] : choices) {
    const Language* l = Find(code);
    if (!l) continue;
    s += std::string(first ? "\n" : ",\n") + "    \"" + std::to_string(id) + "\": \"" + l->code + "\"";
    first = false;
  }
  s += first ? "}\n}\n" : "\n  }\n}\n";
  return s;
}

}  // namespace readyup::lang
