#pragma once

// Which language each player gets Ready Up's text in (docs/ARCHITECTURE.md, API 1.19):
// their own `.lang` choice (saved in csgo/readyup/player_languages.json), else the one a plugin
// set for them (the platform's site language, through fleet), else readyup.cfg `language`
// (default "en"). CS2 does not send the server a client's game language (cl_language is not a
// userinfo convar), so it cannot be read. Codes are the platform's: en, de, pt-BR, zh-CN, ...
// Pure parts are tested by ctest `player_language`.

#include <cstdint>
#include <map>
#include <string>

namespace readyup::lang {

struct Language {
  const char* code;    // "pt-BR"
  const char* cs2;     // CS2's cl_language name, "brazilian"
  const char* native;  // "Português (Brasil)"
};

// Every language Ready Up has tables for, English first.
const Language* Languages(size_t* count);

// The language for a code or CS2 language name in any case ("DE", "pt_br", "schinese", "zh",
// "no"), or nullptr.
const Language* Find(const std::string& codeOrName);

// player_languages.json: {"version": 1, "players": {"<steamid64>": "<code>"}}. Unknown codes are dropped.
std::map<uint64_t, std::string> ParseChoices(const std::string& json);
std::string ChoicesJson(const std::map<uint64_t, std::string>& choices);

// ---- runtime (thread-safe) ----
// The player's language code (never empty; a stable pointer).
const char* ForSteamId(uint64_t steamid64);
// The server's language (readyup.cfg `language`, else "en").
const char* ServerDefault();
// The player's own `.lang` choice, or nullptr.
const Language* Chosen(uint64_t steamid64);
// `.lang <code>` (nullptr clears it). Saved at once.
void SetChosen(uint64_t steamid64, const Language* lang);
// A plugin's language for the player (nullptr clears it). Not saved.
void SetFromPlugin(uint64_t steamid64, const Language* lang);

}  // namespace readyup::lang
