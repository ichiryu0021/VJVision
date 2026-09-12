cmake_minimum_required(VERSION 3.21)
project(VJVision VERSION 2.2.1 LANGUAGES C CXX)

# VJVision 2.0 — C++ rewrite (v1 was Python/dejavu).
# Version single source of truth: project(VERSION ...) on line 2.
# VJVISION_VERSION is force-derived from it and injected into the binary
# as VJVISION_VERSION_STR (util/version.h) so every build is stamped.
set(VJVISION_VERSION "${PROJECT_VERSION}" CACHE STRING "VJVision version string" FORCE)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Prevent MSBuild/CMake from auto-re-running configure during build
# (sandbox file restrictions block the regenerate step).
set(CMAKE_SUPPRESS_REGENERATION TRUE)

# --- Vendored third-party (M0 spike: pffft + sqlite3 amalgamation) ---
# pffft is replaced by our own radix-2 FFT in third_party/pffft/pffft.c
add_library(pffft STATIC third_party/pffft/pffft.cpp)
target_include_directories(pffft PUBLIC third_party/pffft)

# SQLite3 amalgamation
add_library(sqlite3 STATIC third_party/sqlite3_extract/sqlite-amalgamation-3490100/sqlite3.c)
target_include_directories(sqlite3 PUBLIC third_party/sqlite3_extract/sqlite-amalgamation-3490100)
if(MSVC)
    target_compile_definitions(sqlite3 PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION)
endif()

option(VJVC_WITH_QT "Build the Qt Quick visualizer (M3+)" ON)

# Match engine selection: an optional engine can be supplied by a local,
# non-published source tree. When its sources are present it is built as a
# static library and linked in automatically; a clean public clone has no
# proprietary/ directory and builds the open-source confidence engine.
# Pass -DVJVISION_PROPRIETARY=OFF to force the open engine even when a
# local optional engine exists.
set(VJVISION_SRC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/src")
if(NOT DEFINED VJVISION_PROPRIETARY)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/proprietary/chargebar/charge_engine.cpp")
        set(VJVISION_PROPRIETARY ON)
    else()
        set(VJVISION_PROPRIETARY OFF)
    endif()
endif()
message(STATUS "VJVision match engine: "
    "${VJVISION_PROPRIETARY} (ON=local optional engine, OFF=open confidence)")

add_executable(VJVision WIN32
    src/main.cpp
    src/fp/stft.cpp
    src/fp/peaks.cpp
    src/fp/hashing.cpp
    src/fp/fp_db.cpp
    src/fp/align.cpp
    src/audio/wasapi_capture.cpp
    src/audio/spectrum.cpp
    src/audio/beat_tracker.cpp
    src/engine/indexer.cpp
    src/util/tags.cpp
    src/util/audio_file.cpp
    src/viz/ipc_pipe.cpp
    src/viz/viz.qrc
    src/VJVision.rc
)
if(VJVISION_PROPRIETARY)
    add_subdirectory(proprietary/chargebar)
else()
    target_sources(VJVision PRIVATE src/engine/confidence_engine.cpp)
endif()
if(VJVC_WITH_QT)
    target_sources(VJVision PRIVATE
        src/viz/qt_viz.cpp
        src/viz/viz_controller.cpp
        src/ui/prefs.cpp
        src/ui/control_panel.cpp
    )
    set_target_properties(VJVision PROPERTIES AUTOMOC ON AUTORCC ON)
endif()

target_include_directories(VJVision PRIVATE src)

# Stamp every build with the CMake project version (see util/version.h).
target_compile_definitions(VJVision PRIVATE VJVISION_VERSION_STR="${VJVISION_VERSION}")

# ole32: CoCreateInstance / COM (WASAPI device enumerator)
# shell32: CommandLineToArgvW (wide argv → UTF-8)
target_link_libraries(VJVision PRIVATE pffft sqlite3 ole32 shell32)

if(VJVISION_PROPRIETARY)
    target_link_libraries(VJVision PRIVATE vjcharge)
    target_compile_definitions(VJVision PRIVATE VJVISION_CHARGE_ENGINE=1)
endif()

if(VJVC_WITH_QT)
    find_package(Qt6 REQUIRED COMPONENTS Gui Quick Widgets Multimedia)
    target_link_libraries(VJVision PRIVATE Qt6::Gui Qt6::Quick Qt6::Widgets Qt6::Multimedia)
    target_compile_definitions(VJVision PRIVATE VJVC_WITH_QT=1)
endif()

if(MSVC)
    target_compile_options(VJVision PRIVATE /W3 /utf-8 /EHsc)
    target_compile_definitions(VJVision PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    target_compile_options(pffft PRIVATE /W0)
    target_compile_options(sqlite3 PRIVATE /W0)
    if(VJVISION_PROPRIETARY)
        # Keep the closed-source objects optimized together, fold identical
        # COMDTs, and ship a deterministic Release image without debug/PDB
        # linkage. PDB/source/static libraries are never deployment artifacts.
        target_link_options(VJVision PRIVATE
            $<$<CONFIG:Release>:/LTCG>
            $<$<CONFIG:Release>:/OPT:REF>
            $<$<CONFIG:Release>:/OPT:ICF>
            $<$<CONFIG:Release>:/DEBUG:NONE>
            $<$<CONFIG:Release>:/Brepro>
        )
    endif()
endif()
