#pragma once

// Chat color control bytes for CS2 / CounterStrikeSharp-style chat formatting.
//
// These values are intentionally duplicated from:
//   ready-up/counterstrikesharp/managed/CounterStrikeSharp.API/Modules/Utils/ChatColors.cs
//
// Keep names + values identical so it’s easy to cross-reference with upstream.

namespace readyup {

struct ChatColors {
  inline static constexpr char Default = '\x01';
  inline static constexpr char White = '\x01';
  inline static constexpr char DarkRed = '\x02';
  inline static constexpr char Green = '\x04';
  inline static constexpr char LightYellow = '\x09';
  inline static constexpr char LightBlue = '\x0B';
  inline static constexpr char Olive = '\x05';
  inline static constexpr char Lime = '\x06';
  inline static constexpr char Red = '\x07';
  inline static constexpr char LightPurple = '\x03';
  inline static constexpr char Purple = '\x0E';
  inline static constexpr char Grey = '\x08';
  inline static constexpr char Yellow = '\x09';
  inline static constexpr char Gold = '\x10';
  inline static constexpr char Silver = '\x0A';
  inline static constexpr char Blue = '\x0B';
  inline static constexpr char DarkBlue = '\x0C';
  inline static constexpr char BlueGrey = '\x0A';
  inline static constexpr char Magenta = '\x0E';
  inline static constexpr char LightRed = '\x0F';
  inline static constexpr char Orange = '\x10';

  // Compatibility alias present in CounterStrikeSharp.
  inline static constexpr char Darkred = '\x02';
};

}  // namespace readyup

