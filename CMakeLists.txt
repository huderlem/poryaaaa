cmake_minimum_required(VERSION 3.24)
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

# ---- CLAP SDK ----
set(CLAP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(clap-sdk)

# ---- clap-wrapper (standalone support) ----
set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES TRUE CACHE BOOL "" FORCE)
add_subdirectory(clap-wrapper)
# clap-wrapper enables -Werror but its own fsutil.cpp triggers -Wreturn-type
# with MinGW cross-compilation, so relax that specific warning.
# Also ensure _WIN32_WINNT targets Vista+ for SHGetKnownFolderPath.
if(TARGET clap-wrapper-compile-options)
    target_compile_options(clap-wrapper-compile-options INTERFACE -Wno-error=return-type)
    if(WIN32 AND MINGW)
        target_compile_definitions(clap-wrapper-compile-options INTERFACE _WIN32_WINNT=0x0600)
    endif()
endif()

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

target_link_libraries(poryaaaa PRIVATE clap glfw)

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

# ---- Standalone (clap-wrapper) ----
add_executable(poryaaaa-standalone
    plugin/m4a_plugin.c
    plugin/m4a_gui.cpp
    ${ENGINE_SOURCES}
    ${IMGUI_SOURCES}
)
target_include_directories(poryaaaa-standalone PRIVATE
    plugin
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)
set_target_properties(poryaaaa-standalone PROPERTIES
    LINKER_LANGUAGE CXX
)
target_link_libraries(poryaaaa-standalone PRIVATE clap glfw)

if(UNIX AND NOT APPLE)
    target_link_libraries(poryaaaa-standalone PRIVATE m GL)
elseif(APPLE)
    target_link_libraries(poryaaaa-standalone PRIVATE "-framework OpenGL")
elseif(WIN32)
    target_link_libraries(poryaaaa-standalone PRIVATE opengl32 -static-libgcc -static-libstdc++)
endif()

target_add_standalone_wrapper(
    TARGET poryaaaa-standalone
    OUTPUT_NAME "poryaaaa_standalone"
    STATICALLY_LINKED_CLAP_ENTRY TRUE
    PLUGIN_ID "com.huderlem.poryaaaa"
)
set_target_properties(poryaaaa-standalone PROPERTIES OUTPUT_NAME "poryaaaa_standalone")

# clap-wrapper's native Win32 standalone requires WinRT/WIL (MSVC/Clang only).
# For MinGW, replace the generic wrapasstandalone.cpp with our own lightweight
# Win32 entry point that handles GUI hosting and timer firing.
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    get_target_property(_sa_sources poryaaaa-standalone SOURCES)
    list(FILTER _sa_sources EXCLUDE REGEX "wrapasstandalone\\.cpp$")
    set_target_properties(poryaaaa-standalone PROPERTIES SOURCES "${_sa_sources}")
    target_sources(poryaaaa-standalone PRIVATE plugin/standalone_main_win32.cpp)
    target_include_directories(poryaaaa-standalone PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/clap-wrapper/src
    )
endif()
