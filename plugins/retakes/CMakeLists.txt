# readyup-retakes: retakes for fun servers. Spawns come from CS2's own maps/<map>_retake.txt,
# read from the server's pak01_dir.vpk (retakes_rules.h). The plugin itself follows; for now the
# rules and their test.

# VPK reader, retake file parser, teams / queue / scramble, spawns and the planter (ctest
# `retakes_rules`).
add_executable(retakes_rules_test tests/retakes_rules_test.cpp retakes_rules.cpp)
target_include_directories(retakes_rules_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(retakes_rules_test PRIVATE -Wall -Wextra)
add_test(NAME retakes_rules COMMAND retakes_rules_test)
