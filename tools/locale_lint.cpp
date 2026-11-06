// readyup_locale_lint <locales dir>: checks a plugin's locales/*.json against en.json
// (libs/readyup/i18n.h Lint) and that the files are UTF-8 text, not \u escapes (minijson turns
// non-ASCII escapes into '?'). Every readyup_embed_locales() dir runs it as a ctest.
#include "readyup/i18n.h"

#include <dirent.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <locales dir>\n", argv[0]);
    return 2;
  }
  const std::string dir = argv[1];
  DIR* d = opendir(dir.c_str());
  if (!d) {
    std::fprintf(stderr, "cannot open %s\n", dir.c_str());
    return 2;
  }
  std::vector<std::string> codes, texts;
  while (const dirent* e = readdir(d)) {
    const std::string name = e->d_name;
    if (name.size() <= 5 || name.compare(name.size() - 5, 5, ".json") != 0) continue;
    std::ifstream f(dir + "/" + name);
    std::stringstream ss;
    ss << f.rdbuf();
    codes.push_back(name.substr(0, name.size() - 5));
    texts.push_back(ss.str());
  }
  closedir(d);

  int problems = 0;
  std::vector<readyup::i18n::Locale> locales;
  for (size_t i = 0; i < codes.size(); ++i) {
    if (texts[i].find("\\u") != std::string::npos) {
      std::printf("%s.json: write UTF-8 text, not \\u escapes\n", codes[i].c_str());
      ++problems;
    }
    locales.push_back({codes[i].c_str(), texts[i].c_str()});
  }
  readyup::i18n::Catalog c;
  if (const std::string err = c.Load(locales.data(), locales.size()); !err.empty()) {
    std::printf("%s\n", err.c_str());
    ++problems;
  }
  for (const std::string& p : readyup::i18n::Lint(c)) {
    std::printf("%s\n", p.c_str());
    ++problems;
  }
  std::printf("%s: %zu table(s), %d problem(s)\n", dir.c_str(), codes.size(), problems);
  return problems ? 1 : 0;
}
