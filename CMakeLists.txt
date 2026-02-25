cmake_minimum_required(VERSION 3.16)
project(poryaaaa C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# ---- GLFW ----
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
# Disable Wayland on Linux to avoid requiring wayland-scanner at build time;
# the compiled plugin will still use whichever display server the DAW runs on.
set(GLFW_BUILD_WAYLAND  OFF CACHE BOOL "" FORCE)
add_subdirectory(glfw)

# ---- Dear ImGui ----
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

# Common source files (shared between plugin and test)
set(ENGINE_SOURCES
    plugin/m4a_engine.c
    plugin/m4a_channel.c
    plugin/m4a_tables.c
    plugin/m4a_reverb.c
    plugin/voicegroup_loader.c
)

# ---- CLAP Plugin ----
add_library(poryaaaa MODULE
    plugin/m4a_plugin.c
    plugin/m4a_gui.cpp
    ${ENGINE_SOURCES}
    ${IMGUI_SOURCES}
)

target_include_directories(poryaaaa PRIVATE
    clap-sdk/include
    plugin
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

# Use C++ linker (required because we mix C and C++ sources)
set_target_properties(poryaaaa PROPERTIES
    PREFIX ""
    SUFFIX ".clap"
    LINKER_LANGUAGE CXX
)

target_link_libraries(poryaaaa PRIVATE glfw)

if(UNIX AND NOT APPLE)
    target_link_libraries(poryaaaa PRIVATE m)
endif()

# OpenGL
if(APPLE)
    target_link_libraries(poryaaaa PRIVATE "-framework OpenGL")
elseif(WIN32)
    target_link_libraries(poryaaaa PRIVATE opengl32)
else()
    target_link_libraries(poryaaaa PRIVATE GL)
endif()

# Platform-specific settings
if(WIN32)
    target_link_libraries(poryaaaa PRIVATE -static-libgcc -static-libstdc++)
elseif(APPLE)
    set_target_properties(poryaaaa PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "clap"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/plugin/Info.plist"
    )
endif()

# ---- Standalone Test (WAV export) ----
add_executable(poryaaaa_test
    test/test_wav_export.c
    ${ENGINE_SOURCES}
)

target_include_directories(poryaaaa_test PRIVATE
    plugin
)

target_link_libraries(poryaaaa_test PRIVATE m)

# ---- Standalone Renderer ----
add_executable(poryaaaa_render
    cmd/poryaaaa_render.c
    ${ENGINE_SOURCES}
)
target_include_directories(poryaaaa_render PRIVATE plugin third_party)
target_link_libraries(poryaaaa_render PRIVATE m)
if(UNIX AND NOT APPLE)
    target_link_libraries(poryaaaa_render PRIVATE pthread dl)
endif()
if(APPLE)
    target_link_libraries(poryaaaa_render PRIVATE "-framework CoreAudio" "-framework AudioToolbox" "-framework CoreFoundation")
endif()
if(WIN32)
    target_link_libraries(poryaaaa_render PRIVATE -static-libgcc)
endif()

# ---- Unit Tests ----
add_executable(poryaaaa_unit_tests
    test/test_engine.c
    ${ENGINE_SOURCES}
)

target_include_directories(poryaaaa_unit_tests PRIVATE
    plugin
)

target_link_libraries(poryaaaa_unit_tests PRIVATE m)
