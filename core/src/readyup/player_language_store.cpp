// The runtime half of player_language.h: the choices file and the per-player lookup.
#include "readyup/config.h"
#include "readyup/logging.h"
#include "readyup/path.h"
#include "readyup/player_language.h"

#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>

namespace readyup::lang {
namespace {

std::mutex g_mu;
bool g_loaded = false;
std::map<uint64_t, std::string> g_chosen;           // `.lang`, saved
std::map<uint64_t, const Language*> g_fromPlugin;  // set_player_language, not saved

std::string ChoicesPath() {
  const std::string csgo = GetCsgoDirFromModuleDir();
  return csgo.empty() ? std::string() : csgo + "/readyup/player_languages.json";
}

// Must hold g_mu.
void LoadLocked() {
  if (g_loaded) return;
  g_loaded = true;
  std::ifstream f(ChoicesPath());
  if (!f) return;
  std::stringstream ss;
  ss << f.rdbuf();
  g_chosen = ParseChoices(ss.str());
}

// Must hold g_mu.
void SaveLocked() {
  const std::string path = ChoicesPath();
  if (path.empty()) return;
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    f << ChoicesJson(g_chosen);
    if (!f) {
      Print("lang: could not write %s\n", tmp.c_str());
      return;
    }
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) Print("lang: could not save %s\n", path.c_str());
}

}  // namespace

const char* ServerDefault() {
  const Language* l = Find(Cfg().language);
  return l ? l->code : "en";
}

const Language* Chosen(uint64_t steamid64) {
  std::lock_guard<std::mutex> lk(g_mu);
  LoadLocked();
  const auto it = g_chosen.find(steamid64);
  return it == g_chosen.end() ? nullptr : Find(it->second);
}

const char* ForSteamId(uint64_t steamid64) {
  if (steamid64 != 0) {
    if (const Language* l = Chosen(steamid64)) return l->code;
    std::lock_guard<std::mutex> lk(g_mu);
    const auto it = g_fromPlugin.find(steamid64);
    if (it != g_fromPlugin.end()) return it->second->code;
  }
  return ServerDefault();
}

void SetChosen(uint64_t steamid64, const Language* lang) {
  if (steamid64 == 0) return;
  std::lock_guard<std::mutex> lk(g_mu);
  LoadLocked();
  if (lang) g_chosen[steamid64] = lang->code;
  else g_chosen.erase(steamid64);
  SaveLocked();
}

void SetFromPlugin(uint64_t steamid64, const Language* lang) {
  if (steamid64 == 0) return;
  std::lock_guard<std::mutex> lk(g_mu);
  if (lang) g_fromPlugin[steamid64] = lang;
  else g_fromPlugin.erase(steamid64);
}

}  // namespace readyup::lang
