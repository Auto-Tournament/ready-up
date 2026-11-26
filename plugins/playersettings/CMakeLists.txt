# readyup-playersettings -> build/plugins/playersettings.so: reads players' CS2 settings (cvar
# queries). Engine entries: gamedata/engine-surface.playersettings.json. Not in release zips yet
# (step 1: the query and `ru cvar`; packaging comes with the platform side).
readyup_add_plugin(playersettings playersettings_plugin.cpp cvar_msg.cpp)
target_compile_definitions(readyup_plugin_playersettings PRIVATE PLAYERSETTINGS_VERSION="${READYUP_SEMVER}")

# The query / answer messages as raw memory (cvar_msg.h; ctest `playersettings_cvar_msg`).
add_executable(playersettings_cvar_msg_test tests/cvar_msg_test.cpp cvar_msg.cpp)
target_include_directories(playersettings_cvar_msg_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(playersettings_cvar_msg_test PRIVATE -Wall -Wextra)
add_test(NAME playersettings_cvar_msg COMMAND playersettings_cvar_msg_test)
