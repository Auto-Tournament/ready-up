// text.h's lookup, engine-free (the card tests link it with the embedded tables).
#include "readyup/text.h"

#include <cstdio>

extern const readyup::i18n::Locale kReadyUpLocales[];  // plugins/match/locales (readyup_embed_locales)
extern const size_t kReadyUpLocaleCount;

namespace readyup::text {
namespace {

const i18n::Catalog& Catalog() {
  static const i18n::Catalog c = [] {
    i18n::Catalog x;
    if (const std::string err = x.Load(kReadyUpLocales, kReadyUpLocaleCount); !err.empty())
      std::fprintf(stderr, "match: locales: %s\n", err.c_str());
    return x;
  }();
  return c;
}

}  // namespace

std::string ForLang(const std::string& lang, const std::string& key, const Args& args) {
  return Catalog().Tr(lang, key, args);
}

}  // namespace readyup::text
