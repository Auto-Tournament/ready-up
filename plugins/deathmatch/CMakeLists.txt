# readyup-deathmatch -> build/plugins/deathmatch.so (csgo/readyup/plugins/deathmatch.so): CS2's
# deathmatch game mode (free for all / team deathmatch) with kill + time limits, a leaderboard
# panel, spawn protection, headshot only and weapon rounds. Talks to the match plugin through
# readyup.match.v1 set_external_mode and to essentials through readyup.essentials.v1 (default
# maps). Shares the map entry parsing with the match plugin. Ships in the Full bundle only.
readyup_add_plugin(deathmatch deathmatch_plugin.cpp dm_rules.cpp ${CMAKE_SOURCE_DIR}/plugins/match/readyup/map_names.cpp)
target_include_directories(readyup_plugin_deathmatch PRIVATE ${CMAKE_SOURCE_DIR}/plugins/match)
target_compile_definitions(readyup_plugin_deathmatch PRIVATE DEATHMATCH_VERSION="${READYUP_SEMVER}")

# Settings, cvars, scoring, win conditions, leaderboard ordering + HTML escaping, weapon rounds,
# map resolution (dm_rules.h; ctest `deathmatch_rules`).
add_executable(deathmatch_rules_test tests/dm_rules_test.cpp dm_rules.cpp)
target_include_directories(deathmatch_rules_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(deathmatch_rules_test PRIVATE -Wall -Wextra)
add_test(NAME deathmatch_rules COMMAND deathmatch_rules_test)
