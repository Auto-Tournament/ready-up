# readyup-addons -> build/plugins/addons.so (csgo/readyup/plugins/addons.so): Steam Workshop addons
# for the server (cfg/ReadyUp/addons.cfg). Engine entries: gamedata/engine-surface.addons.json.
readyup_add_plugin(addons addons_plugin.cpp addons_rules.cpp addons_vpk.cpp)
target_compile_definitions(readyup_plugin_addons PRIVATE ADDONS_VERSION="${READYUP_SEMVER}")
target_link_libraries(readyup_plugin_addons PRIVATE dl)

# Workshop id list and per-item actions (addons_rules.h; ctest `addons_rules`).
add_executable(addons_rules_test tests/addons_rules_test.cpp addons_rules.cpp)
target_include_directories(addons_rules_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(addons_rules_test PRIVATE -Wall -Wextra)
add_test(NAME addons_rules COMMAND addons_rules_test)

# VPK directory parsing and the extraction filter (addons_vpk.h; ctest `addons_vpk`).
add_executable(addons_vpk_test tests/addons_vpk_test.cpp addons_vpk.cpp)
target_include_directories(addons_vpk_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(addons_vpk_test PRIVATE -Wall -Wextra)
add_test(NAME addons_vpk COMMAND addons_vpk_test)
