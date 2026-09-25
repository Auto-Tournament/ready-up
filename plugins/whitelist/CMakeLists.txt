# readyup-whitelist -> build/plugins/whitelist.so (csgo/readyup/plugins/whitelist.so): only the
# listed players may stay on a practice / scrim server. Off until `ru whitelist on`.
readyup_add_plugin(whitelist whitelist_plugin.cpp whitelist_rules.cpp ${CMAKE_SOURCE_DIR}/libs/readyup/minijson.cpp)
target_include_directories(readyup_plugin_whitelist PRIVATE ${CMAKE_SOURCE_DIR}/libs)
target_compile_definitions(readyup_plugin_whitelist PRIVATE WHITELIST_VERSION="${READYUP_SEMVER}")

# whitelist.json and who gets kicked (whitelist_rules.h; ctest `whitelist_rules`).
add_executable(whitelist_rules_test tests/whitelist_rules_test.cpp whitelist_rules.cpp ${CMAKE_SOURCE_DIR}/libs/readyup/minijson.cpp)
target_include_directories(whitelist_rules_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/libs)
target_compile_options(whitelist_rules_test PRIVATE -Wall -Wextra)
add_test(NAME whitelist_rules COMMAND whitelist_rules_test)
