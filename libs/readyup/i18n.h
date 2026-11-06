#pragma once

// Player-facing text in the player's language. Each plugin keeps flat JSON tables in
// <plugin>/locales/<code>.json ({"key": "text with {placeholder}"}), compiled into the .so by
// readyup_embed_locales() in CMake; en.json is the source every other table follows.
//
// Lookup: the player's code ("pt-BR"), then its base ("pt"), then "en", then the key itself.
// Placeholders are {name}. With a "count" argument, key_one / key_few / key_many / key_other
// are tried first (the CLDR category of count in that language, as i18next names them).
// ctest `i18n`; `readyup_locale_lint <dir>` checks a locales dir.

#include <cstddef>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace readyup::i18n {

// One embedded table: its code and its JSON text.
struct Locale {
  const char* code;
  const char* json;
};

using Args = std::vector<std::pair<std::string, std::string>>;

class Catalog {
 public:
  // Parses every table. Returns "" or what was wrong with the first bad one (that one is skipped).
  std::string Load(const Locale* locales, size_t n);
  // The text of `key` for `lang` with args filled in (see the top of this file).
  std::string Tr(const std::string& lang, const std::string& key, const Args& args = {}) const;
  bool HasLanguage(const std::string& lang) const { return tables_.count(lang) > 0; }

  using Table = std::unordered_map<std::string, std::string>;
  const std::unordered_map<std::string, Table>& tables() const { return tables_; }

 private:
  const std::string* Find(const std::string& lang, const std::string& key, const Args& args) const;
  std::unordered_map<std::string, Table> tables_;
};

// CLDR cardinal category of the integer n in lang: "one", "few", "many" or "other".
const char* PluralCategory(const std::string& lang, long long n);

// tmpl with every {name} from args replaced; unknown placeholders stay as they are.
std::string Format(const std::string& tmpl, const Args& args);

// Problems in a catalog, one line each: a table that is not "en" with a key en does not have
// (plural variants of an en key are fine), or whose {placeholders} differ from en's.
std::vector<std::string> Lint(const Catalog& catalog);

}  // namespace readyup::i18n
