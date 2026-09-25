# readyup-midas -> build/plugins/midas.so (csgo/readyup/plugins/midas.so): a fun plugin. Weapons
# picked up by the players in `midas_steamids` turn gold (the entity's m_clrRender). Off by
# default, never under the valve ruleset. Ships in the Full bundle only.
readyup_add_plugin(midas midas_plugin.cpp midas_rules.cpp)
target_compile_definitions(readyup_plugin_midas PRIVATE MIDAS_VERSION="${READYUP_SEMVER}")

# Config parsing and the tint decision (midas_rules.h; ctest `midas_rules`).
add_executable(midas_rules_test tests/midas_rules_test.cpp midas_rules.cpp)
target_include_directories(midas_rules_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(midas_rules_test PRIVATE -Wall -Wextra)
add_test(NAME midas_rules COMMAND midas_rules_test)
