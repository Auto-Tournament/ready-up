# readyup-match -> build/plugins/match.so (csgo/readyup/plugins/match.so): the match flow
# (ready-up, scrims, knife, pauses, practice, match configs, webhooks, demos, stats).
# Talks to the core only through ru_api (core/include/readyup); engine-free shared code comes
# from libs/readyup. Needs libcurl (webhooks, `ru match load`, demo upload); without it those
# fall back to the `curl` binary. Settings, match recovery state and admins are JSON files in the
# plugin data dir (readyup/local_store.h, docs/FLEET.md D13).
set(MATCH_SOURCES
  readyup/admin_check.cpp
  readyup/admins.cpp
  readyup/backup_files.cpp
  readyup/config.cpp
  readyup/cs2_version.cpp
  readyup/damage_ledger.cpp
  readyup/damage_report.cpp
  readyup/demo_naming.cpp
  readyup/demo_recorder.cpp
  readyup/esports.cpp
  readyup/fleet_bridge.cpp
  readyup/fleet_state.cpp
  readyup/game_timers.cpp
  readyup/host.cpp
  readyup/idle_refresh.cpp
  readyup/knife_tracker.cpp
  readyup/local_store.cpp
  readyup/ru_commands.cpp
  readyup/map_names.cpp
  readyup/mat_admins.cpp
  readyup/match_config_parser.cpp
  readyup/match_console.cpp
  readyup/match_end.cpp
  readyup/match_end_logic.cpp
  readyup/match_events.cpp
  readyup/match_features.cpp
  readyup/match_log.cpp
  readyup/match_plugin.cpp
  readyup/match_recovery.cpp
  readyup/match_rules.cpp
  readyup/match_router.cpp
  readyup/match_signals.cpp
  readyup/match_state.cpp
  readyup/match_stats.cpp
  readyup/match_status.cpp
  readyup/match_token.cpp
  readyup/modes.cpp
  readyup/pause_state.cpp
  readyup/persisted_match_state.cpp
  readyup/persisted_settings.cpp
  readyup/player_registry.cpp
  readyup/practice_tools.cpp
  readyup/ready_hud.cpp
  readyup/reload_state.cpp
  readyup/ruleset.cpp
  readyup/scrim_flow.cpp
  readyup/vote_logic.cpp
  readyup/votes.cpp
  readyup/webhook.cpp
  readyup/welcome.cpp
  readyup/workers.cpp
  # Shared engine-free code (libs/readyup).
  ${CMAKE_SOURCE_DIR}/libs/readyup/http_client.cpp
  ${CMAKE_SOURCE_DIR}/libs/readyup/json_store.cpp
  ${CMAKE_SOURCE_DIR}/libs/readyup/minijson.cpp
  ${CMAKE_SOURCE_DIR}/libs/readyup/status_snapshot.cpp
  ${CMAKE_SOURCE_DIR}/libs/readyup/steamid.cpp)

readyup_add_plugin(match ${MATCH_SOURCES})
# plugins/match first: its readyup/config.h, logging.h, ... are what libs/ sources include here.
target_include_directories(readyup_plugin_match BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/libs)
target_compile_definitions(readyup_plugin_match PRIVATE MATCH_VERSION="${READYUP_SEMVER}")
target_link_libraries(readyup_plugin_match PRIVATE pthread dl)

# libcurl.
if (READYUP_DEPS_PREFIX)
  target_compile_definitions(readyup_plugin_match PRIVATE CURL_STATICLIB READYUP_CURL_CA_PROBE=1)
  target_link_libraries(readyup_plugin_match PRIVATE
    "${READYUP_DEPS_PREFIX}/lib/libcurl.a" "${READYUP_DEPS_PREFIX}/lib/libssl.a"
    "${READYUP_DEPS_PREFIX}/lib/libcrypto.a" m)
elseif (CURL_FOUND)
  if (TARGET CURL::libcurl)
    target_link_libraries(readyup_plugin_match PRIVATE CURL::libcurl)
  else()
    target_include_directories(readyup_plugin_match PRIVATE ${CURL_INCLUDE_DIRS})
    target_link_libraries(readyup_plugin_match PRIVATE ${CURL_LIBRARIES})
  endif()
else()
  target_compile_definitions(readyup_plugin_match PRIVATE READYUP_NO_CURL=1)
  message(WARNING "libcurl dev files not found; readyup-match falls back to the `curl` binary at runtime.")
endif()

# match.so inside the real plugin host with a fake engine API: commands, readyup.match.v1,
# `ru plugin reload match` keeping the scrim / the loaded match, unload (ctest `match_host`).
add_executable(match_host_test tests/match_host_test.cpp ${CMAKE_SOURCE_DIR}/core/src/readyup/plugin_loader.cpp
  ${CMAKE_SOURCE_DIR}/core/src/readyup/plugin_state.cpp ${CMAKE_SOURCE_DIR}/libs/readyup/minijson.cpp)
target_include_directories(match_host_test PRIVATE ${CMAKE_SOURCE_DIR}/core/src ${CMAKE_SOURCE_DIR}/libs
  ${CMAKE_SOURCE_DIR}/core/include)
target_compile_options(match_host_test PRIVATE -Wall -Wextra)
target_link_libraries(match_host_test PRIVATE dl pthread)
add_test(NAME match_host COMMAND match_host_test $<TARGET_FILE:readyup_plugin_match>)
set_tests_properties(match_host PROPERTIES TIMEOUT 120)

# The engine-free half of the fleet match link (readyup/fleet_state.h: merge patch + live_rev,
# epoch fence, config_rev CAS, match.assign -> MAT config through the real parser, update ops,
# sha256 / base64, validators, resume blocks), workshop map names (readyup/map_names.h) and the stats rewind
# of round restores (ctest `match_fleet_state`).
add_executable(match_fleet_state_test tests/fleet_state_test.cpp readyup/fleet_state.cpp readyup/map_names.cpp
  readyup/match_stats.cpp readyup/match_config_parser.cpp readyup/match_rules.cpp readyup/ruleset.cpp ${CMAKE_SOURCE_DIR}/libs/readyup/minijson.cpp
  ${CMAKE_SOURCE_DIR}/libs/readyup/status_snapshot.cpp ${CMAKE_SOURCE_DIR}/libs/readyup/steamid.cpp)
target_include_directories(match_fleet_state_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/libs
  ${CMAKE_SOURCE_DIR}/core/include)
target_compile_options(match_fleet_state_test PRIVATE -Wall -Wextra)
add_test(NAME match_fleet_state COMMAND match_fleet_state_test)

# Pause limits / unpause rule / auto-unpause countdown, .forceready threshold, team name
# sanitizing and the team-left forfeit timer (readyup/match_rules.h; ctest `match_rules`).
add_executable(match_rules_test tests/match_rules_test.cpp readyup/match_rules.cpp)
target_include_directories(match_rules_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(match_rules_test PRIVATE -Wall -Wextra)
add_test(NAME match_rules COMMAND match_rules_test)

# Damage report bookkeeping (readyup/damage_ledger.h), the .gg / .stop vote logic
# (readyup/vote_logic.h) and their rules (ctest `match_damage_votes`).
add_executable(match_damage_votes_test tests/damage_votes_test.cpp readyup/damage_ledger.cpp readyup/vote_logic.cpp
  readyup/match_rules.cpp)
target_include_directories(match_damage_votes_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(match_damage_votes_test PRIVATE -Wall -Wextra)
add_test(NAME match_damage_votes COMMAND match_damage_votes_test)

# Rulesets: presets, override parsing / validation, preset + overrides resolution, what differs,
# the commands after the go-live cfg, knife refusal (readyup/ruleset.h; ctest `match_ruleset`).
add_executable(match_ruleset_test tests/ruleset_test.cpp readyup/ruleset.cpp readyup/match_rules.cpp
  readyup/match_config_parser.cpp ${CMAKE_SOURCE_DIR}/libs/readyup/minijson.cpp
  ${CMAKE_SOURCE_DIR}/libs/readyup/status_snapshot.cpp ${CMAKE_SOURCE_DIR}/libs/readyup/steamid.cpp)
target_include_directories(match_ruleset_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/libs
  ${CMAKE_SOURCE_DIR}/core/include)
target_compile_options(match_ruleset_test PRIVATE -Wall -Wextra)
add_test(NAME match_ruleset COMMAND match_ruleset_test)

# `ru` main commands + subcommands of the match plugin (readyup/ru_commands.h): the help, the
# lookup, `.ru map change` / `.ru match load` parsing, `ru map reload`'s entry for workshop maps
# (ctest `match_ru_commands`).
add_executable(match_ru_commands_test tests/ru_commands_test.cpp readyup/ru_commands.cpp readyup/map_names.cpp)
target_include_directories(match_ru_commands_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(match_ru_commands_test PRIVATE -Wall -Wextra)
target_link_libraries(match_ru_commands_test PRIVATE pthread)
add_test(NAME match_ru_commands COMMAND match_ru_commands_test)

# Idle map refresh decision (readyup/idle_refresh.h; ctest `match_idle_refresh`).
add_executable(match_idle_refresh_test tests/idle_refresh_test.cpp)
target_include_directories(match_idle_refresh_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(match_idle_refresh_test PRIVATE -Wall -Wextra)
add_test(NAME match_idle_refresh COMMAND match_idle_refresh_test)
