# readyup-hud -> build/plugins/hud.so (csgo/readyup/plugins/hud.so): custom in-game HUDs through
# CS2's custom_hud_layout entity, drawing the Auto Tournament HUD workshop addon (repo hud/).
# Other plugins use it through readyup.hud.v1 (core/include/readyup/hud_iface.h).
# Engine entries: gamedata/engine-surface.hud.json.
readyup_add_plugin(hud hud_plugin.cpp hud_vec.cpp hud_screen.cpp)
target_include_directories(readyup_plugin_hud PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(readyup_plugin_hud PRIVATE HUD_VERSION="${READYUP_SEMVER}")
target_link_libraries(readyup_plugin_hud PRIVATE dl)

# Raw CUtlVector helpers (hud_vec.h; ctest `hud_vec`).
add_executable(hud_vec_test tests/hud_vec_test.cpp hud_vec.cpp)
target_include_directories(hud_vec_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(hud_vec_test PRIVATE -Wall -Wextra)
add_test(NAME hud_vec COMMAND hud_vec_test)

# Screen specs for the clickable HUD (hud_screen.h; ctest `hud_screen`).
add_executable(hud_screen_test tests/hud_screen_test.cpp hud_screen.cpp)
target_include_directories(hud_screen_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(hud_screen_test PRIVATE -Wall -Wextra)
add_test(NAME hud_screen COMMAND hud_screen_test)
