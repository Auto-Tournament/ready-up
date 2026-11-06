#pragma once

// readyup-match's player-facing text in each player's language: plugins/match/locales/<code>.json
// (libs/readyup/i18n.h), the language from ru_api 1.19 player_language (English on older cores).
// Like engine.h's chat helpers, the senders may be called from any thread (queued to the next
// frame there).

#include "readyup/i18n.h"

#include <string>

namespace readyup::text {

using Args = i18n::Args;

// The text of `key` in `lang`.
std::string ForLang(const std::string& lang, const std::string& key, const Args& args = {});
// The text of `key` for the player in `slot` (-1: the server's language). Game thread.
std::string For(int slot, const std::string& key, const Args& args = {});

// SendToChat: everyone, each in their own language.
void ChatAll(const std::string& key, Args args = {});
// SendImportantToChat: gold, three times, and in the center for `centerSeconds` (0: chat only).
void ImportantAll(const std::string& key, Args args = {}, int centerSeconds = 12);
// ClientPrintChat with the Ready Up prefix: one player.
void ChatSlot(int slot, const std::string& key, Args args = {});

}  // namespace readyup::text
