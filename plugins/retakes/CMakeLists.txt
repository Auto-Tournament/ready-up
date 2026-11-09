# readyup-retakes -> build/plugins/retakes.so (csgo/readyup/plugins/retakes.so): retakes for fun
# servers. Spawns come from CS2's own maps/<map>_retake.txt, read from the server's pak01_dir.vpk
# (retakes_rules.h). Runs the server through readyup.match.v1 set_external_mode("retakes").
readyup_add_plugin(retakes retakes_plugin.cpp retakes_rules.cpp)
target_include_directories(readyup_plugin_retakes PRIVATE ${CMAKE_SOURCE_DIR}/libs)
readyup_embed_locales(readyup_plugin_retakes ${CMAKE_CURRENT_SOURCE_DIR}/locales)
target_compile_definitions(readyup_plugin_retakes PRIVATE RETAKES_VERSION="${READYUP_SEMVER}")

# VPK reader, retake file parser, teams / queue / scramble, spawns and the planter (ctest
# `retakes_rules`).
add_executable(retakes_rules_test tests/retakes_rules_test.cpp retakes_rules.cpp)
target_include_directories(retakes_rules_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(retakes_rules_test PRIVATE -Wall -Wextra)
add_test(NAME retakes_rules COMMAND retakes_rules_test)
