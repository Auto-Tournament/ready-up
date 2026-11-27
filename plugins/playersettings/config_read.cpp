#include "config_read.h"

#include <cstdio>

namespace playersettings {

const std::vector<std::string>& ConfigCvars() {
  static const std::vector<std::string> kCvars = {
      // Mouse
      "sensitivity", "zoom_sensitivity_ratio",
      // Crosshair (the code carries the rest)
      "cl_crosshairstyle", "cl_crosshairsize", "cl_crosshairthickness", "cl_crosshairdot", "cl_crosshair_drawoutline",
      "cl_crosshair_t", "cl_crosshaircolor_r", "cl_crosshaircolor_g", "cl_crosshaircolor_b", "cl_crosshairalpha",
      // Viewmodel
      "viewmodel_fov", "viewmodel_offset_x", "viewmodel_offset_y", "viewmodel_offset_z", "viewmodel_presetpos",
      "cl_prefer_lefthanded",
      // HUD and radar
      "hud_scaling", "cl_hud_color", "cl_radar_scale", "cl_radar_always_centered", "cl_radar_rotate", "cl_hud_radar_scale",
      "cl_radar_square_with_scoreboard",
      // Video
      "fps_max"};
  return kCvars;
}

bool IsCrosshairCode(const std::string& code) {
  auto alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };
  // Today's CS2 code: "CS" + 44.
  if (code.size() == 46 && code.compare(0, 2, "CS") == 0) {
    for (size_t i = 2; i < code.size(); ++i)
      if (!alnum(code[i])) return false;
    return true;
  }
  // The old container: CSGO + 5 groups of "-xxxxx".
  if (code.size() != 4 + 5 * 6 || code.compare(0, 4, "CSGO") != 0) return false;
  for (size_t g = 0; g < 5; ++g) {
    const size_t at = 4 + g * 6;
    if (code[at] != '-') return false;
    for (size_t i = 1; i <= 5; ++i) {
      const char c = code[at + i];
      const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
      if (!ok) return false;
    }
  }
  return true;
}

namespace {

void AppendString(std::string& out, const std::string& s) {
  out += '"';
  for (const char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += static_cast<char>(c);
    } else if (c < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof buf, "\\u%04x", c);
      out += buf;
    } else {
      out += static_cast<char>(c);
    }
  }
  out += '"';
}

}  // namespace

std::string ConfigPayload(uint64_t steamid64, const std::string& code, const std::map<std::string, std::string>& cvars) {
  std::string out = "{\"steamid64\":\"" + std::to_string(steamid64) + "\"";
  if (IsCrosshairCode(code)) out += ",\"crosshair_code\":\"" + code + "\"";
  out += ",\"cvars\":{";
  bool first = true;
  for (const auto& [name, value] : cvars) {
    if (!first) out += ',';
    first = false;
    AppendString(out, name);
    out += ':';
    // Cut on a byte boundary is fine: every value we keep is a number.
    AppendString(out, value.substr(0, 32));
  }
  out += "}}";
  return out;
}

}  // namespace playersettings
