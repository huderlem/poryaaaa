cmake_minimum_required(VERSION 3.24)
project(poryaaaa C CXX)

# Objective-C/C++ are only needed for the macOS Pugl backend (mac.m, mac_gl.m).
# Enabling them unconditionally in project() forces every platform to have an
# Objective-C compiler, which breaks Linux/Windows configuration.
if(APPLE)
    enable_language(OBJC)
    enable_language(OBJCXX)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Default to an optimized build when the user hasn't picked a configuration.
# This is a realtime audio engine; an unoptimized (-O0) default would run the
# per-sample DSP roughly an order of magnitude slower than it needs to.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING
        "Build type (Debug, Release, RelWithDebInfo, MinSizeRel)" FORCE)
    message(STATUS "No CMAKE_BUILD_TYPE set; defaulting to Release")
endif()

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

# ---- Pugl ----
set(PUGL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pugl)

# Platform-specific Pugl sources
if(WIN32)
    set(PUGL_PLATFORM_SOURCES
        ${PUGL_DIR}/src/win.c
        ${PUGL_DIR}/src/win_gl.c
        ${PUGL_DIR}/src/win_stub.c
    )
elseif(APPLE)
    set(PUGL_PLATFORM_SOURCES
        ${PUGL_DIR}/src/mac.m
        ${PUGL_DIR}/src/mac_gl.m
        ${PUGL_DIR}/src/mac_stub.m
    )
else()
    set(PUGL_PLATFORM_SOURCES
        ${PUGL_DIR}/src/x11.c
        ${PUGL_DIR}/src/x11_gl.c
        ${PUGL_DIR}/src/x11_stub.c
    )
endif()

set(PUGL_SOURCES
    ${PUGL_DIR}/src/common.c
    ${PUGL_DIR}/src/internal.c
    ${PUGL_PLATFORM_SOURCES}
)

# ---- Dear ImGui ----
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    plugin/imgui_impl_pugl.cpp
)

# Common source files (shared between plugin and test)
set(ENGINE_SOURCES
    plugin/m4a_engine.c
    plugin/m4a_channel.c
    plugin/m4a_tables.c
    plugin/m4a_mixer.c
    plugin/m4a_blip.c
    plugin/m4a_gba.c
    plugin/m4a_psg.c
    plugin/m4a_song.c
    plugin/m4a_sequencer.c
    plugin/m4a_cry.c
    plugin/voicegroup_loader.c
)

# ---- CLAP Plugin ----
add_library(poryaaaa MODULE
    plugin/m4a_plugin.c
    plugin/m4a_gui.cpp
    ${ENGINE_SOURCES}
    ${IMGUI_SOURCES}
    ${PUGL_SOURCES}
)

target_include_directories(poryaaaa PRIVATE
    plugin
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${PUGL_DIR}/include
)

# Build Pugl as a static library (not a DLL) on all platforms
target_compile_definitions(poryaaaa PRIVATE PUGL_STATIC)

# Use C++ linker (required because we mix C and C++ sources)
set_target_properties(poryaaaa PROPERTIES
    PREFIX ""
    SUFFIX ".clap"
    LINKER_LANGUAGE CXX
)

target_link_libraries(poryaaaa PRIVATE clap)

if(UNIX AND NOT APPLE)
    target_link_libraries(poryaaaa PRIVATE m X11 Xcursor Xrandr Xext)
endif()

# OpenGL
if(APPLE)
    target_link_libraries(poryaaaa PRIVATE "-framework OpenGL" "-framework Cocoa" "-framework CoreVideo")
elseif(WIN32)
    target_link_libraries(poryaaaa PRIVATE opengl32)
else()
    target_link_libraries(poryaaaa PRIVATE GL)
endif()

# Platform-specific settings
if(WIN32)
    target_link_libraries(poryaaaa PRIVATE dwmapi -static-libgcc -static-libstdc++)
elseif(APPLE)
    set_target_properties(poryaaaa PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "clap"
    )
    # MACOSX_BUNDLE_INFO_PLIST only works for MACOSX_BUNDLE (app bundles).
    # For a generic BUNDLE (.clap), copy Info.plist manually.
    add_custom_command(TARGET poryaaaa POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/plugin/Info.plist"
            "$<TARGET_BUNDLE_CONTENT_DIR:poryaaaa>/Info.plist"
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
    cmd/gba_tempo_grid.c
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
    cmd/gba_tempo_grid.c
    ${ENGINE_SOURCES}
)

target_include_directories(poryaaaa_unit_tests PRIVATE
    plugin
    cmd
)

target_link_libraries(poryaaaa_unit_tests PRIVATE m)

# Byte-exact check of the .s song parser against the accuracy test ROM
# (needs the ROM build; skips with status 2 when it is absent).
add_executable(poryaaaa_song_tests
    test/test_song_parser.c
    plugin/m4a_song.c
)
target_include_directories(poryaaaa_song_tests PRIVATE plugin)

# ---- Standalone (clap-wrapper) ----
add_executable(poryaaaa-standalone
    plugin/m4a_plugin.c
    plugin/m4a_gui.cpp
    ${ENGINE_SOURCES}
    ${IMGUI_SOURCES}
    ${PUGL_SOURCES}
)
target_include_directories(poryaaaa-standalone PRIVATE
    plugin
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${PUGL_DIR}/include
)
set_target_properties(poryaaaa-standalone PROPERTIES
    LINKER_LANGUAGE CXX
)

# Build Pugl as a static library (not a DLL) on all platforms
target_compile_definitions(poryaaaa-standalone PRIVATE PUGL_STATIC)
target_link_libraries(poryaaaa-standalone PRIVATE clap)

if(UNIX AND NOT APPLE)
    target_link_libraries(poryaaaa-standalone PRIVATE m GL X11 Xcursor Xrandr Xext)
elseif(APPLE)
    target_link_libraries(poryaaaa-standalone PRIVATE "-framework OpenGL" "-framework Cocoa" "-framework CoreVideo")
elseif(WIN32)
    target_link_libraries(poryaaaa-standalone PRIVATE opengl32 dwmapi -static-libgcc -static-libstdc++)
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

if(UNIX AND NOT APPLE)
    get_target_property(_sa_sources_lin poryaaaa-standalone SOURCES)
    list(FILTER _sa_sources_lin EXCLUDE REGEX "wrapasstandalone\\.cpp$")
    set_target_properties(poryaaaa-standalone PROPERTIES SOURCES "${_sa_sources_lin}")
    target_sources(poryaaaa-standalone PRIVATE plugin/standalone_main_linux.cpp)
    target_include_directories(poryaaaa-standalone PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/clap-wrapper/src
    )

    # Our standalone uses Pugl (not GTK), so strip GTK3 from the
    # clap-wrapper standalone library to prevent assert(gtkGui) crash
    # when gtkmm-3.0 happens to be installed on the build system.
    set(_salib poryaaaa-standalone-clap-wrapper-standalone-lib)
    get_target_property(_salib_sources ${_salib} SOURCES)
    if(_salib_sources)
        list(FILTER _salib_sources EXCLUDE REGEX "gtkutils\\.cpp$")
        set_target_properties(${_salib} PROPERTIES SOURCES "${_salib_sources}")
    endif()
    foreach(_prop COMPILE_DEFINITIONS INTERFACE_COMPILE_DEFINITIONS)
        get_target_property(_defs ${_salib} ${_prop})
        if(_defs)
            list(REMOVE_ITEM _defs "CLAP_WRAPPER_HAS_GTK3")
            set_target_properties(${_salib} PROPERTIES ${_prop} "${_defs}")
        endif()
    endforeach()
endif()

# ---- Accuracy harness (optional; see docs/ACCURACY_PLAN.md, test/accuracy/README.md) ----
# Enabled only when the pokeemerald tree (with agbcc and its tools built) and an
# mgba source tree with build/libmgba-static.a are provided:
#   cmake -B build -DACCURACY_POKEEMERALD_DIR=~/pokeemerald -DACCURACY_MGBA_DIR=~/mgba
#   cmake --build build --target accuracy
set(ACCURACY_POKEEMERALD_DIR "" CACHE PATH "pokeemerald tree used to build the accuracy test ROM")
set(ACCURACY_MGBA_DIR "" CACHE PATH "mgba source tree containing build/libmgba-static.a")

if(ACCURACY_MGBA_DIR)
    add_executable(mgba_render test/accuracy/ref/mgba_render.c plugin/m4a_blip.c)
    target_include_directories(mgba_render PRIVATE
        ${CMAKE_SOURCE_DIR}/plugin
        ${ACCURACY_MGBA_DIR}/include
        ${ACCURACY_MGBA_DIR}/build/include
    )
    target_link_libraries(mgba_render PRIVATE ${ACCURACY_MGBA_DIR}/build/libmgba-static.a z png m pthread)
    if(UNIX AND NOT APPLE)
        target_link_libraries(mgba_render PRIVATE rt dl)
    endif()
    # Only needed when libmgba was built with its GL/EGL features (the default).
    foreach(_lib EGL GL)
        find_library(_accuracy_${_lib} ${_lib})
        if(_accuracy_${_lib})
            target_link_libraries(mgba_render PRIVATE ${_accuracy_${_lib}})
        endif()
    endforeach()

    # Bit-exactness test of the engine's resampler against blip_buf's fast
    # linear path (the LGPL reference is only ever linked into this test).
    add_executable(poryaaaa_blip_tests
        test/test_blip.c
        plugin/m4a_blip.c
    plugin/m4a_gba.c
        ${ACCURACY_MGBA_DIR}/src/third-party/blip_buf/blip_buf.c
    )
    target_include_directories(poryaaaa_blip_tests PRIVATE
        plugin
        ${ACCURACY_MGBA_DIR}/include
        ${ACCURACY_MGBA_DIR}/build/include
    )
endif()

if(ACCURACY_POKEEMERALD_DIR)
    set(ACCURACY_ROM ${CMAKE_BINARY_DIR}/accuracy/rom/accuracy_test.gba)
    add_custom_target(accuracy_rom
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/accuracy/rom
        COMMAND $(MAKE) -C ${CMAKE_CURRENT_SOURCE_DIR}/test/accuracy/rom
                POKEEMERALD=${ACCURACY_POKEEMERALD_DIR} BUILD=${CMAKE_BINARY_DIR}/accuracy/rom
        COMMENT "Building the accuracy test ROM with agbcc"
        VERBATIM
    )
    add_custom_target(accuracy_song_tests
        COMMAND $<TARGET_FILE:poryaaaa_song_tests> ${CMAKE_BINARY_DIR}/accuracy/rom
        DEPENDS accuracy_rom poryaaaa_song_tests
        COMMENT "Accuracy: checking the .s song parser against the ROM"
        VERBATIM
    )
endif()

if(ACCURACY_POKEEMERALD_DIR AND ACCURACY_MGBA_DIR)
    add_custom_target(accuracy
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/test/accuracy/tools/accuracy.py
                --pokeemerald ${ACCURACY_POKEEMERALD_DIR}
                --rom ${ACCURACY_ROM}
                --mgba-render $<TARGET_FILE:mgba_render>
                --poryaaaa-render $<TARGET_FILE:poryaaaa_render>
                --build-dir ${CMAKE_BINARY_DIR}/accuracy
        DEPENDS accuracy_rom accuracy_song_tests mgba_render poryaaaa_render
        COMMENT "Accuracy: rendering references with mgba and comparing poryaaaa"
        USES_TERMINAL
        VERBATIM
    )
endif()
