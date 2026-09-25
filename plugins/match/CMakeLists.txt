# readyup-match -> build/plugins/match.so (csgo/readyup/plugins/match.so): the match flow
# (ready-up, scrims, knife, pauses, practice, match configs, webhooks, Postgres, demos, stats).
# Talks to the core only through ru_api (core/include/readyup); engine-free shared code comes
# from libs/readyup. Needs libcurl (webhooks, `ru match load`, demo upload) and libpq
# (persisted settings / match state, admins); without them those features are compiled out,
# as they were in the core.
set(MATCH_SOURCES
  readyup/admin_check.cpp
  readyup/admins.cpp
  readyup/backup_files.cpp
  readyup/config.cpp
  readyup/cs2_version.cpp
  readyup/db_config_file.cpp
  readyup/db_writer.cpp
  readyup/demo_naming.cpp
  readyup/demo_recorder.cpp
  readyup/fleet_bridge.cpp
  readyup/fleet_state.cpp
  readyup/game_timers.cpp
  readyup/host.cpp
  readyup/knife_tracker.cpp
  readyup/mat_admins.cpp
  readyup/match_config_parser.cpp
  readyup/match_console.cpp
  readyup/match_end.cpp
  readyup/match_end_logic.cpp
  readyup/match_events.cpp
  readyup/match_log.cpp
  readyup/match_plugin.cpp
  readyup/match_recovery.cpp
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
  readyup/ready_hud.cpp
  readyup/reload_state.cpp
  readyup/scrim_flow.cpp
  readyup/webhook.cpp
  readyup/welcome.cpp
  readyup/workers.cpp
  # Shared engine-free code (libs/readyup).
  ${CMAKE_SOURCE_DIR}/libs/readyup/db_config.cpp
  ${CMAKE_SOURCE_DIR}/libs/readyup/http_client.cpp
  ${CMAKE_SOURCE_DIR}/libs/readyup/minijson.cpp
  ${CMAKE_SOURCE_DIR}/libs/readyup/postgres.cpp
  ${CMAKE_SOURCE_DIR}/libs/readyup/status_snapshot.cpp
  ${CMAKE_SOURCE_DIR}/libs/readyup/steamid.cpp)

readyup_add_plugin(match ${MATCH_SOURCES})
# plugins/match first: its readyup/config.h, logging.h, ... are what libs/ sources include here.
target_include_directories(readyup_plugin_match BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/libs)
target_compile_definitions(readyup_plugin_match PRIVATE MATCH_VERSION="${READYUP_SEMVER}")
target_link_libraries(readyup_plugin_match PRIVATE pthread dl)

# libpq (same linkage as readyup_libs_db: static in release / docker builds).
if (READYUP_DEPS_PREFIX)
  target_include_directories(readyup_plugin_match PRIVATE "${READYUP_DEPS_PREFIX}/include")
elseif (PostgreSQL_FOUND)
  target_include_directories(readyup_plugin_match PRIVATE ${PostgreSQL_INCLUDE_DIRS})
else()
  target_compile_definitions(readyup_plugin_match PRIVATE READYUP_NO_POSTGRES=1)
endif()
if (READYUP_DEPS_PREFIX OR PostgreSQL_FOUND)
  target_link_libraries(readyup_plugin_match PRIVATE readyup_libs_db)
endif()

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
add_executable(match_host_test tests/match_host_test.cpp ${CMAKE_SOURCE_DIR}/core/src/readyup/plugin_loader.cpp)
target_include_directories(match_host_test PRIVATE ${CMAKE_SOURCE_DIR}/core/src ${CMAKE_SOURCE_DIR}/libs
  ${CMAKE_SOURCE_DIR}/core/include)
target_compile_options(match_host_test PRIVATE -Wall -Wextra)
target_link_libraries(match_host_test PRIVATE dl pthread)
add_test(NAME match_host COMMAND match_host_test $<TARGET_FILE:readyup_plugin_match>)
set_tests_properties(match_host PROPERTIES TIMEOUT 120)

# The engine-free half of the fleet match link (readyup/fleet_state.h: merge patch + live_rev,
# epoch fence, config_rev CAS, match.assign -> MAT config through the real parser, update ops,
# sha256 / base64, validators) and the stats rewind of round restores (ctest `match_fleet_state`).
add_executable(match_fleet_state_test tests/fleet_state_test.cpp readyup/fleet_state.cpp readyup/match_stats.cpp
  readyup/match_config_parser.cpp ${CMAKE_SOURCE_DIR}/libs/readyup/minijson.cpp
  ${CMAKE_SOURCE_DIR}/libs/readyup/status_snapshot.cpp ${CMAKE_SOURCE_DIR}/libs/readyup/steamid.cpp)
target_include_directories(match_fleet_state_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/libs
  ${CMAKE_SOURCE_DIR}/core/include)
target_compile_options(match_fleet_state_test PRIVATE -Wall -Wextra)
add_test(NAME match_fleet_state COMMAND match_fleet_state_test)
