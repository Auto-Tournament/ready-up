#include "readyup/i18n.h"

#include "readyup/minijson.h"

#include <cstdlib>
#include <set>

namespace readyup::i18n {
namespace {

const char* const kPluralSuffixes[] = {"_one", "_few", "_many", "_other"};

std::string BaseOf(const std::string& lang) {
  const size_t dash = lang.find('-');
  return dash == std::string::npos ? lang : lang.substr(0, dash);
}

const std::string* CountArg(const Args& args) {
  for (const auto& a : args) {
    if (a.first == "count") return &a.second;
  }
  return nullptr;
}

std::set<std::string> Placeholders(const std::string& s) {
  std::set<std::string> out;
  for (size_t i = s.find('{'); i != std::string::npos; i = s.find('{', i + 1)) {
    const size_t end = s.find('}', i);
    if (end == std::string::npos) break;
    out.insert(s.substr(i + 1, end - i - 1));
  }
  return out;
}

// The en key a key is checked against: a plural variant ("x_few") against en's "x_other";
// *plural is set for those (their {count} may come and go: en "one player", ru "{count} игрок").
std::string EnKeyFor(const Catalog::Table& en, const std::string& key, bool* plural) {
  *plural = false;
  for (const char* suf : kPluralSuffixes) {
    const std::string s(suf);
    if (key.size() > s.size() && key.compare(key.size() - s.size(), s.size(), s) == 0) {
      const std::string stem = key.substr(0, key.size() - s.size());
      // en counts it with plural keys, or with one key and a {count} other languages may inflect.
      for (const std::string& k : {stem + "_other", stem}) {
        if (en.count(k)) {
          *plural = true;
          return k;
        }
      }
    }
  }
  return en.count(key) ? key : std::string();
}

}  // namespace

std::string Catalog::Load(const Locale* locales, size_t n) {
  std::string firstError;
  for (size_t i = 0; i < n; ++i) {
    minijson::ParseError err;
    const auto v = minijson::Parse(locales[i].json ? locales[i].json : "", &err);
    if (!v || !minijson::IsObject(&*v)) {
      if (firstError.empty())
        firstError = std::string(locales[i].code) + ".json: " + (v ? "not an object" : err.msg) + " at " +
                     std::to_string(err.offset);
      continue;
    }
    Table t;
    for (const auto& kv : v->obj) {
      if (minijson::IsString(&kv.second)) t[kv.first] = kv.second.str;
    }
    tables_[locales[i].code] = std::move(t);
  }
  return firstError;
}

const std::string* Catalog::Find(const std::string& lang, const std::string& key, const Args& args) const {
  const std::string* count = CountArg(args);
  for (const std::string& code : {lang, BaseOf(lang), std::string("en")}) {
    const auto t = tables_.find(code);
    if (t == tables_.end()) continue;
    if (count) {
      const std::string cat = PluralCategory(code, std::atoll(count->c_str()));
      for (const std::string& k : {key + "_" + cat, key + "_other"}) {
        const auto it = t->second.find(k);
        if (it != t->second.end()) return &it->second;
      }
    }
    const auto it = t->second.find(key);
    if (it != t->second.end()) return &it->second;
  }
  return nullptr;
}

std::string Catalog::Tr(const std::string& lang, const std::string& key, const Args& args) const {
  const std::string* s = Find(lang, key, args);
  return Format(s ? *s : key, args);
}

const char* PluralCategory(const std::string& lang, long long n) {
  if (n < 0) n = -n;
  const std::string b = BaseOf(lang);
  const long long m10 = n % 10, m100 = n % 100;
  const bool few234 = m10 >= 2 && m10 <= 4 && !(m100 >= 12 && m100 <= 14);
  if (b == "ja" || b == "ko" || b == "zh" || b == "th" || b == "vi" || b == "id") return "other";
  if (b == "fr" || lang == "pt-BR" || lang == "pt") return n <= 1 ? "one" : "other";
  if (b == "ru" || b == "uk") {
    if (m10 == 1 && m100 != 11) return "one";
    return few234 ? "few" : "many";
  }
  if (b == "pl") {
    if (n == 1) return "one";
    return few234 ? "few" : "many";
  }
  if (b == "cs") return n == 1 ? "one" : (n >= 2 && n <= 4) ? "few" : "other";
  if (b == "ro") return n == 1 ? "one" : (n == 0 || (m100 >= 2 && m100 <= 19)) ? "few" : "other";
  return n == 1 ? "one" : "other";  // en de nl sv da nb fi it es el hu bg tr pt-PT
}

std::string Format(const std::string& tmpl, const Args& args) {
  std::string out;
  out.reserve(tmpl.size());
  for (size_t i = 0; i < tmpl.size(); ++i) {
    if (tmpl[i] == '{') {
      const size_t end = tmpl.find('}', i);
      if (end != std::string::npos) {
        const std::string name = tmpl.substr(i + 1, end - i - 1);
        const std::string* val = nullptr;
        for (const auto& a : args) {
          if (a.first == name) val = &a.second;
        }
        if (val) {
          out += *val;
          i = end;
          continue;
        }
      }
    }
    out.push_back(tmpl[i]);
  }
  return out;
}

std::vector<std::string> Lint(const Catalog& catalog) {
  std::vector<std::string> out;
  const auto en = catalog.tables().find("en");
  if (en == catalog.tables().end()) return {"no en table"};
  for (const auto& [code, table] : catalog.tables()) {
    if (code == "en") continue;
    for (const auto& [key, text] : table) {
      bool plural = false;
      const auto it = en->second.find(EnKeyFor(en->second, key, &plural));
      if (it == en->second.end()) {
        out.push_back(code + ": " + key + " is not in en");
        continue;
      }
      auto mine = Placeholders(text), theirs = Placeholders(it->second);
      if (plural) {
        mine.erase("count");
        theirs.erase("count");
      }
      if (mine != theirs)
        out.push_back(code + ": " + key + " has other {placeholders} than en");
    }
  }
  return out;
}

}  // namespace readyup::i18n
