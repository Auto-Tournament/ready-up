# readyup-practice -> build/plugins/practice.so (csgo/readyup/plugins/practice.so): practice mode
# and its tools, with or without the match plugin (a dedicated practice server: always=1).
readyup_add_plugin(practice practice_plugin.cpp practice_rules.cpp practice_feedback.cpp practice_tools.cpp
  practice_lineups.cpp
  # Shared engine-free code (libs/readyup): the lineup files.
  ${CMAKE_SOURCE_DIR}/libs/readyup/json_store.cpp
  ${CMAKE_SOURCE_DIR}/libs/readyup/status_snapshot.cpp)
target_include_directories(readyup_plugin_practice PRIVATE ${CMAKE_SOURCE_DIR}/libs)
target_compile_definitions(readyup_plugin_practice PRIVATE PRACTICE_VERSION="${READYUP_SEMVER}")

# Tool / bot commands, when they may run, always=1, the lineup book (practice_rules.h,
# practice_tools.h, practice_lineups.h; ctest `practice_rules`).
add_executable(practice_rules_test tests/practice_rules_test.cpp practice_rules.cpp practice_feedback.cpp
  practice_tools.cpp practice_lineups.cpp ${CMAKE_SOURCE_DIR}/libs/readyup/status_snapshot.cpp)
target_include_directories(practice_rules_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/libs)
target_compile_options(practice_rules_test PRIVATE -Wall -Wextra)
add_test(NAME practice_rules COMMAND practice_rules_test)
