# readyup-essentials -> build/plugins/essentials.so: server basics apart from the match flow
# (admins, map change / reload / restart). Shares the map entry parsing with the match plugin.
readyup_add_plugin(essentials essentials_plugin.cpp essentials_rules.cpp
  ${CMAKE_SOURCE_DIR}/plugins/match/readyup/map_names.cpp ${CMAKE_SOURCE_DIR}/libs/readyup/status_snapshot.cpp)
target_include_directories(readyup_plugin_essentials PRIVATE ${CMAKE_SOURCE_DIR}/plugins/match ${CMAKE_SOURCE_DIR}/libs)
target_compile_definitions(readyup_plugin_essentials PRIVATE ESSENTIALS_VERSION="${READYUP_SEMVER}")

# admins.json, name matching, when map commands are refused (essentials_rules.h; ctest `essentials_rules`).
add_executable(essentials_rules_test tests/essentials_rules_test.cpp essentials_rules.cpp
  ${CMAKE_SOURCE_DIR}/libs/readyup/status_snapshot.cpp)
target_include_directories(essentials_rules_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/libs)
target_compile_options(essentials_rules_test PRIVATE -Wall -Wextra)
add_test(NAME essentials_rules COMMAND essentials_rules_test)
