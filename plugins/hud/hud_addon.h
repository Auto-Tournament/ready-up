// The Auto Tournament HUD addon's version (Workshop 3827905393, repo hud/).
//
// Bump it with every addon publish. hud/tools/gen_ui.py reads it from this line and writes the
// matching rules into hud/styles/at_ui.css; the ctest `hud_addon` fails when the two disagree.
//
// How players learn their addon is old: at_ui has a notice (#ui_ver) that is visible unless its
// CSS knows the version class the server puts on it. hud.so always sets `v-<kAddonVersion>`;
// each addon hides the notice for its own version and every older one. So an addon older than
// the server keeps the notice up, in the player's language, and a newer addon stays quiet.
// Addons published before version 1 have no notice at all.
#pragma once

namespace hud {
constexpr int kAddonVersion = 3;
}  // namespace hud
