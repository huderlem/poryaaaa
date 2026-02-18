cmake_minimum_required(VERSION 3.16)
project(m4a_plugin C)

set(CMAKE_C_STANDARD 11)

# Common source files (shared between plugin and test)
set(ENGINE_SOURCES
    plugin/m4a_engine.c
    plugin/m4a_channel.c
    plugin/m4a_tables.c
    plugin/m4a_reverb.c
    plugin/voicegroup_loader.c
)

# ---- CLAP Plugin ----
add_library(m4a_plugin MODULE
    plugin/m4a_plugin.c
    ${ENGINE_SOURCES}
)

target_include_directories(m4a_plugin PRIVATE
    clap-sdk/include
    plugin
)

set_target_properties(m4a_plugin PROPERTIES
    PREFIX ""
    SUFFIX ".clap"
)

# Platform-specific settings
if(WIN32)
    target_link_libraries(m4a_plugin PRIVATE -static-libgcc)
elseif(APPLE)
    set_target_properties(m4a_plugin PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "clap"
    )
endif()

# ---- Standalone Test (WAV export) ----
add_executable(m4a_test
    test/test_wav_export.c
    ${ENGINE_SOURCES}
)

target_include_directories(m4a_test PRIVATE
    plugin
)

target_link_libraries(m4a_test PRIVATE m)

# ---- Unit Tests ----
add_executable(m4a_unit_tests
    test/test_engine.c
    ${ENGINE_SOURCES}
)

target_include_directories(m4a_unit_tests PRIVATE
    plugin
)

target_link_libraries(m4a_unit_tests PRIVATE m)
