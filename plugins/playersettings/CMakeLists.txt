# readyup-playersettings -> build/plugins/playersettings.so: reads players' CS2 config (cvar queries
# and the crosshair code) for the platform's profile Config tab. Engine entries:
# gamedata/engine-surface.playersettings.json. In the full bundle and its own zip.
readyup_add_plugin(playersettings playersettings_plugin.cpp cvar_msg.cpp config_read.cpp)
target_compile_definitions(readyup_plugin_playersettings PRIVATE PLAYERSETTINGS_VERSION="${READYUP_SEMVER}")

# The query / answer messages as raw memory (cvar_msg.h; ctest `playersettings_cvar_msg`).
add_executable(playersettings_cvar_msg_test tests/cvar_msg_test.cpp cvar_msg.cpp)
target_include_directories(playersettings_cvar_msg_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(playersettings_cvar_msg_test PRIVATE -Wall -Wextra)
add_test(NAME playersettings_cvar_msg COMMAND playersettings_cvar_msg_test)

# The config read: its cvar list and the `player.config` payload (ctest `playersettings_config_read`).
add_executable(playersettings_config_read_test tests/config_read_test.cpp config_read.cpp)
target_include_directories(playersettings_config_read_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(playersettings_config_read_test PRIVATE -Wall -Wextra)
add_test(NAME playersettings_config_read COMMAND playersettings_config_read_test)
