# readyup-skins -> build/plugins/skins.so (csgo/readyup/plugins/skins.so). Talks to the core only
# through ru_api; engine entries it needs live in gamedata/engine-surface.skins.json, which the
# core merges at load when the file sits next to it (shipped only in the skins packages).
readyup_add_plugin(skins
  skins_plugin.cpp
  loadout.cpp
  apply.cpp
  cosmetics.cpp
  stattrak.cpp)
target_link_libraries(readyup_plugin_skins PRIVATE readyup_libs_db)
target_compile_definitions(readyup_plugin_skins PRIVATE SKINS_VERSION="${READYUP_SEMVER}")
target_compile_options(readyup_plugin_skins PRIVATE -Wpedantic)
