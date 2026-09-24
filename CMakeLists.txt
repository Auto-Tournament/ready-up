cmake_minimum_required(VERSION 3.20)
project(readyup_shim LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Embed gamedata/engine-surface.json into the shim (runtime prefers an on-disk copy next to
# the shim if one exists). Re-run configure whenever the JSON changes.
set(READYUP_ENGINE_SURFACE_JSON "${CMAKE_SOURCE_DIR}/gamedata/engine-surface.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${READYUP_ENGINE_SURFACE_JSON}")
file(READ "${READYUP_ENGINE_SURFACE_JSON}" READYUP_ENGINE_SURFACE_JSON_TEXT)
set(READYUP_ENGINE_SURFACE_CPP "${CMAKE_BINARY_DIR}/generated/engine_surface_embedded.cpp")
file(WRITE "${READYUP_ENGINE_SURFACE_CPP}.tmp"
  "// Generated from gamedata/engine-surface.json by CMake. Do not edit.\n"
  "extern const char kReadyUpEmbeddedEngineSurfaceJson[];\n"
  "const char kReadyUpEmbeddedEngineSurfaceJson[] = R\"RUJSON(${READYUP_ENGINE_SURFACE_JSON_TEXT})RUJSON\";\n")
configure_file("${READYUP_ENGINE_SURFACE_CPP}.tmp" "${READYUP_ENGINE_SURFACE_CPP}" COPYONLY)
set_source_files_properties("${READYUP_ENGINE_SURFACE_CPP}" PROPERTIES COMPILE_OPTIONS "-Wno-overlength-strings")

add_library(server_shim SHARED
  core/src/readyup/admin_check.cpp
  core/src/readyup/admins.cpp
  core/src/readyup/backup_files.cpp
  core/src/readyup/banner.cpp
  core/src/readyup/ccommand.cpp
  core/src/readyup/chat.cpp
  core/src/readyup/center_html.cpp
  core/src/readyup/cs2_version.cpp
  core/src/readyup/client_print.cpp
  core/src/readyup/client_command_hook.cpp
  core/src/readyup/command_buffer_hook.cpp
  core/src/readyup/config.cpp
  core/src/readyup/console_command.cpp
  core/src/readyup/crash_handler.cpp
  libs/readyup/db_config.cpp
  core/src/readyup/disabled.cpp
  core/src/readyup/features.cpp
  core/src/readyup/game_frame_hook.cpp
  core/src/readyup/game_events.cpp
  core/src/readyup/host_say_hook.cpp
  libs/readyup/http_client.cpp
  core/src/readyup/log_receiver.cpp
  core/src/readyup/logging.cpp
  core/src/readyup/mat_admins.cpp
  core/src/readyup/match_config_parser.cpp
  core/src/readyup/match_recovery.cpp
  core/src/readyup/match_token.cpp
  libs/readyup/minijson.cpp
  core/src/readyup/match_state.cpp
  core/src/readyup/modes.cpp
  core/src/readyup/persisted_settings.cpp
  core/src/readyup/persisted_match_state.cpp
  core/src/readyup/path.cpp
  core/src/readyup/pause_state.cpp
  core/src/readyup/player_registry.cpp
  libs/readyup/postgres.cpp
  core/src/readyup/real_server.cpp
  core/src/readyup/round_termination_hook.cpp
  core/src/readyup/ru_help.cpp
  core/src/readyup/ru_router.cpp
  core/src/readyup/server_game_clients_hook.cpp
  core/src/readyup/schema.cpp
  core/src/readyup/scrim_flow.cpp
  core/src/readyup/selftest.cpp
  core/src/readyup/signature_scan.cpp
  core/src/readyup/sigtest.cpp
  core/src/readyup/slot_registry.cpp
  libs/readyup/steamid.cpp
  core/src/readyup/webhook.cpp
  core/src/readyup/welcome.cpp
  core/src/readyup/ready_hud.cpp
  core/src/readyup/knife_tracker.cpp
  core/src/readyup/weapon_paints.cpp
  core/src/readyup/weapon_paints_apply.cpp
  core/src/readyup/weapon_paints_cosmetics.cpp
  core/src/readyup/skins_engine.cpp
  core/src/readyup/plugin_loader.cpp
  core/src/readyup/plugin_engine_api.cpp
  core/src/readyup/engine_surface.cpp
  core/src/readyup/engine_surface_core.cpp
  ${READYUP_ENGINE_SURFACE_CPP}
  # core/src/exports.cpp is added below and MUST stay last: its __attribute__((constructor))
  # (readyup_ctor) is placed in .init_array in link order, and it must run AFTER every other
  # TU's static initializers. When it was first, later initializers re-constructed globals the
  # constructor had already filled in (e.g. the loaded gamedata string, the "disabled" flag).
  # scripts/check_shim_binary.sh verifies this.
)

# Vendored upstream funchook v1.1.3 + diStorm 3.5.2 (instruction-relocating detours).
# Built as a separate static PIC library so our strict warning flags don't apply to third-party code.
add_library(readyup_funchook STATIC
  third_party/funchook/src/funchook.c
  third_party/funchook/src/funchook_unix.c
  third_party/funchook/src/funchook_x86.c
  third_party/funchook/src/disasm_distorm.c
  third_party/distorm/src/decoder.c
  third_party/distorm/src/distorm.c
  third_party/distorm/src/instructions.c
  third_party/distorm/src/insts.c
  third_party/distorm/src/mnemonics.c
  third_party/distorm/src/operands.c
  third_party/distorm/src/prefix.c
  third_party/distorm/src/textdefs.c
)
set_target_properties(readyup_funchook PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
target_include_directories(readyup_funchook
  PUBLIC ${CMAKE_SOURCE_DIR}/third_party/funchook/include
  PRIVATE ${CMAKE_SOURCE_DIR}/third_party/funchook/config
          ${CMAKE_SOURCE_DIR}/third_party/distorm/include)
target_compile_options(readyup_funchook PRIVATE -w)
target_link_libraries(readyup_funchook PRIVATE dl)

# Build/version strings for `ru`.
# SemVer is sourced from VERSION file, git SHA is appended as build metadata.
set(READYUP_SEMVER "unknown")
if (EXISTS "${CMAKE_SOURCE_DIR}/VERSION")
  file(READ "${CMAKE_SOURCE_DIR}/VERSION" READYUP_SEMVER)
  string(STRIP "${READYUP_SEMVER}" READYUP_SEMVER)
endif()

execute_process(
  COMMAND git rev-parse --short HEAD
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE READYUP_GIT_SHA
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)
if (NOT READYUP_GIT_SHA)
  set(READYUP_GIT_SHA "unknown")
endif()

set(READYUP_BUILD_VERSION "${READYUP_SEMVER} (${READYUP_GIT_SHA})")
target_compile_definitions(server_shim PRIVATE
  READYUP_SEMVER="${READYUP_SEMVER}"
  READYUP_BUILD_VERSION="${READYUP_BUILD_VERSION}"
)

# Produce the artifact name the engine expects.
set_target_properties(server_shim PROPERTIES
  OUTPUT_NAME "libserver"
  PREFIX ""
)

target_compile_options(server_shim PRIVATE
  -fvisibility=hidden
  -Wall
  -Wextra
  -Wpedantic
)

target_include_directories(server_shim PRIVATE
  ${CMAKE_SOURCE_DIR}/core/src
  ${CMAKE_SOURCE_DIR}/libs
  ${CMAKE_SOURCE_DIR}/core/include
  ${CMAKE_SOURCE_DIR}  # "third_party/funchook/include/funchook.h"
)

# READYUP_STATIC_LIBPQ: link libpq statically so the shim does not need
# libpq.so.5 on the game server host. Needs libpq.a (libpq-dev) plus the PIC
# pgcommon/pgport archives (postgresql-server-dev-NN). Used by
# scripts/docker-build.sh.
option(READYUP_STATIC_LIBPQ "Link libpq statically" OFF)

# READYUP_DEPS_PREFIX: release builds (scripts/sniper-build.sh, CI). Points at a
# prefix produced by scripts/ci/build-static-deps.sh holding static PIC OpenSSL,
# libpq and libcurl. Everything (plus libstdc++/libgcc) is linked in and hidden,
# so the shim only needs glibc (<= 2.31, Steam Runtime sniper) on the host.
set(READYUP_DEPS_PREFIX "" CACHE PATH "Prefix with static OpenSSL/libpq/libcurl (release builds)")

if (READYUP_DEPS_PREFIX)
  foreach(lib pq pgcommon_shlib pgport_shlib curl ssl crypto)
    if (NOT EXISTS "${READYUP_DEPS_PREFIX}/lib/lib${lib}.a")
      message(FATAL_ERROR "READYUP_DEPS_PREFIX: missing ${READYUP_DEPS_PREFIX}/lib/lib${lib}.a (run scripts/ci/build-static-deps.sh)")
    endif()
  endforeach()
  target_include_directories(server_shim PRIVATE "${READYUP_DEPS_PREFIX}/include")
  target_compile_definitions(server_shim PRIVATE CURL_STATICLIB READYUP_CURL_CA_PROBE=1)
  target_link_libraries(server_shim PRIVATE
    "${READYUP_DEPS_PREFIX}/lib/libpq.a"
    "${READYUP_DEPS_PREFIX}/lib/libpgcommon_shlib.a"
    "${READYUP_DEPS_PREFIX}/lib/libpgport_shlib.a"
    "${READYUP_DEPS_PREFIX}/lib/libcurl.a"
    "${READYUP_DEPS_PREFIX}/lib/libssl.a"
    "${READYUP_DEPS_PREFIX}/lib/libcrypto.a"
    pthread dl m)
  # libstdc++ is linked statically by hand (see the end of this file) instead of via
  # -static-libstdc++, because the driver appends that after every object and its static
  # initializers would then run after readyup_ctor.
  # The dynamic symbol table is pinned by core/src/exports.map (the engine entry points only);
  # everything else, including the static deps and std:: template instances, stays local
  # so nothing interposes on the engine's own copies. (--exclude-libs,ALL would also hide
  # exports.cpp, which is linked from an archive below.)
  target_link_options(server_shim PRIVATE
    -nostdlib++ -static-libgcc
    "LINKER:--version-script=${CMAKE_SOURCE_DIR}/core/src/exports.map"
    "LINKER:--no-undefined")
  set_property(TARGET server_shim APPEND PROPERTY LINK_DEPENDS "${CMAKE_SOURCE_DIR}/core/src/exports.map")
  message(STATUS "Linking static release deps from ${READYUP_DEPS_PREFIX}")
else()
  find_package(PostgreSQL QUIET)
  find_package(CURL QUIET)
endif()

if (READYUP_DEPS_PREFIX)
  # libpq linked above.
elseif (PostgreSQL_FOUND AND READYUP_STATIC_LIBPQ)
  execute_process(COMMAND pg_config --pkglibdir OUTPUT_VARIABLE PG_PKGLIBDIR OUTPUT_STRIP_TRAILING_WHITESPACE)
  find_library(PQ_STATIC NAMES libpq.a REQUIRED)
  find_library(PGCOMMON_SHLIB NAMES libpgcommon_shlib.a HINTS ${PG_PKGLIBDIR} REQUIRED)
  find_library(PGPORT_SHLIB NAMES libpgport_shlib.a HINTS ${PG_PKGLIBDIR} REQUIRED)
  target_include_directories(server_shim PRIVATE ${PostgreSQL_INCLUDE_DIRS})
  target_link_libraries(server_shim PRIVATE
    ${PQ_STATIC} ${PGCOMMON_SHLIB} ${PGPORT_SHLIB}
    ssl crypto gssapi_krb5 ldap lber pthread)
  # Keep libpq symbols out of our dynamic symbol table.
  target_link_options(server_shim PRIVATE "LINKER:--exclude-libs,ALL")
  message(STATUS "Linking libpq statically: ${PQ_STATIC}")
elseif (PostgreSQL_FOUND)
  target_include_directories(server_shim PRIVATE ${PostgreSQL_INCLUDE_DIRS})
  target_link_libraries(server_shim PRIVATE ${PostgreSQL_LIBRARIES})
else()
  target_compile_definitions(server_shim PRIVATE READYUP_NO_POSTGRES=1)
  message(WARNING "PostgreSQL dev files not found; building without Postgres support (READYUP_NO_POSTGRES=1).")
endif()

if (READYUP_DEPS_PREFIX)
  # libcurl linked above.
elseif (CURL_FOUND)
  if (TARGET CURL::libcurl)
    target_link_libraries(server_shim PRIVATE CURL::libcurl)
  else()
    target_include_directories(server_shim PRIVATE ${CURL_INCLUDE_DIRS})
    target_link_libraries(server_shim PRIVATE ${CURL_LIBRARIES})
  endif()
else()
  target_compile_definitions(server_shim PRIVATE READYUP_NO_CURL=1)
  message(WARNING "libcurl dev files not found; building without libcurl (READYUP_NO_CURL=1). URL fetch will fall back to `curl` binary at runtime if available.")
endif()

target_link_libraries(server_shim PRIVATE readyup_funchook dl)

# readyup_ctor's translation unit goes last on the link line (see the source list above).
if (READYUP_DEPS_PREFIX)
  # Static release build: libstdc++.a's own initializers must also precede readyup_ctor, so
  # exports.cpp is built as a one-member archive and linked whole-archive AFTER libstdc++.a
  # (plain objects always precede libraries on the link line). libstdc++.a is listed again
  # after it for anything only exports.o pulls in; the post-build check fails if that ever
  # drags an initializer in behind readyup_ctor.
  if (CMAKE_VERSION VERSION_LESS 3.24)
    message(FATAL_ERROR "READYUP_DEPS_PREFIX builds need CMake >= 3.24 ($<LINK_LIBRARY:WHOLE_ARCHIVE>)")
  endif()
  execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libstdc++.a
    OUTPUT_VARIABLE READYUP_LIBSTDCXX_A OUTPUT_STRIP_TRAILING_WHITESPACE)
  if (NOT IS_ABSOLUTE "${READYUP_LIBSTDCXX_A}" OR NOT EXISTS "${READYUP_LIBSTDCXX_A}")
    message(FATAL_ERROR "static libstdc++.a not found for ${CMAKE_CXX_COMPILER}")
  endif()
  add_library(readyup_exports STATIC core/src/exports.cpp)
  foreach(prop COMPILE_DEFINITIONS INCLUDE_DIRECTORIES COMPILE_OPTIONS)
    set_property(TARGET readyup_exports PROPERTY ${prop} "$<TARGET_PROPERTY:server_shim,${prop}>")
  endforeach()
  set_target_properties(readyup_exports PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_link_libraries(server_shim PRIVATE
    "${READYUP_LIBSTDCXX_A}" "$<LINK_LIBRARY:WHOLE_ARCHIVE,readyup_exports>" "${READYUP_LIBSTDCXX_A}" m)
else()
  target_sources(server_shim PRIVATE core/src/exports.cpp)
endif()

# Post-build guard: fail the build if the shim contains direct calls to __cxa_pure_virtual
# (devirtualized calls through internal-linkage interface mirrors) or if readyup_ctor is not
# the last .init_array entry. Skips itself if binutils are unavailable.
add_custom_command(TARGET server_shim POST_BUILD
  COMMAND sh "${CMAKE_SOURCE_DIR}/scripts/check_shim_binary.sh" "$<TARGET_FILE:server_shim>"
  VERBATIM)

# Offline checker: verifies gamedata/engine-surface.json against a Valve libserver.so on disk
# using the same code the shim runs at load. Usage:
#   build/readyup_sigcheck <path/to/libserver.so> gamedata/engine-surface.json
add_executable(readyup_sigcheck
  tools/sigcheck.cpp
  core/src/readyup/engine_surface_core.cpp
  libs/readyup/minijson.cpp
)
target_include_directories(readyup_sigcheck PRIVATE ${CMAKE_SOURCE_DIR}/core/src ${CMAKE_SOURCE_DIR}/libs)
target_compile_options(readyup_sigcheck PRIVATE -Wall -Wextra)

# Offline hook-site checker: for entries marked "hook": "funchook", resolves the function and
# runs funchook_prepare (prologue decode + trampoline relocation, no install) against a
# mapped copy of Valve's libserver.so. Usage:
#   build/readyup_hookcheck <path/to/libserver.so> gamedata/engine-surface.json
add_executable(readyup_hookcheck
  tools/hookcheck.cpp
  core/src/readyup/engine_surface_core.cpp
  libs/readyup/minijson.cpp
)
target_include_directories(readyup_hookcheck PRIVATE ${CMAKE_SOURCE_DIR}/core/src ${CMAKE_SOURCE_DIR}/libs ${CMAKE_SOURCE_DIR})
target_compile_options(readyup_hookcheck PRIVATE -Wall -Wextra)
target_link_libraries(readyup_hookcheck PRIVATE readyup_funchook)

# ---- Plugins (docs/ARCHITECTURE.md) -----------------------------------------------
# The public C ABI. Plugins link only this: no core sources, no engine SDK mirrors, no
# funchook, no gamedata. Anything engine-facing is a function pointer in ru_api.
add_library(readyup_plugin_api INTERFACE)
target_include_directories(readyup_plugin_api INTERFACE ${CMAKE_SOURCE_DIR}/core/include)

# readyup_add_plugin(<name> <sources...>)
#   -> target readyup_plugin_<name>, artifact ${CMAKE_BINARY_DIR}/plugins/<name>.so
#   (deployed to csgo/readyup/plugins/<name>.so). Build one plugin with:
#   cmake --build build --target readyup_plugin_<name>
function(readyup_add_plugin name)
  set(tgt readyup_plugin_${name})
  add_library(${tgt} MODULE ${ARGN})
  target_link_libraries(${tgt} PRIVATE readyup_plugin_api)
  set_target_properties(${tgt} PROPERTIES
    OUTPUT_NAME "${name}"
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
  target_compile_options(${tgt} PRIVATE -Wall -Wextra
    # STB_GNU_UNIQUE symbols make dlclose a no-op, which would break hot reload.
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:GNU>>:-fno-gnu-unique>)
  # Everything a plugin needs from the core arrives through ru_api; an unresolved symbol
  # at link time means it reached for core internals.
  target_link_options(${tgt} PRIVATE "LINKER:--no-undefined")
endfunction()

add_subdirectory(plugins/hello)

# Offline plugin-host test (no CS2 server): the real loader + stubbed core functions,
# loading hello.so, then hot-reloading a second build of it with a different version.
#   cmake --build build && (cd build && ctest --output-on-failure)
readyup_add_plugin(hello_reload_test plugins/hello/hello.c)
set_target_properties(readyup_plugin_hello_reload_test PROPERTIES
  OUTPUT_NAME "hello"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test")
target_compile_definitions(readyup_plugin_hello_reload_test PRIVATE HELLO_VERSION="1.0.1-reloaded")

add_executable(readyup_plugin_host_test tools/plugin_host_test.cpp core/src/readyup/plugin_loader.cpp)
target_include_directories(readyup_plugin_host_test PRIVATE ${CMAKE_SOURCE_DIR}/core/src ${CMAKE_SOURCE_DIR}/libs ${CMAKE_SOURCE_DIR}/core/include)
target_compile_options(readyup_plugin_host_test PRIVATE -Wall -Wextra)
target_link_libraries(readyup_plugin_host_test PRIVATE dl)

enable_testing()
add_test(NAME plugin_host_reload
  COMMAND readyup_plugin_host_test
    $<TARGET_FILE:readyup_plugin_hello> $<TARGET_FILE:readyup_plugin_hello_reload_test>)
