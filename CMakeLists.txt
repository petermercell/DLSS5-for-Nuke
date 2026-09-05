cmake_minimum_required(VERSION 3.20)
project(DLSS5Live VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Option to also build worker executable (Windows / mingw-w64 only)
option(BUILD_DLSS_WORKER "Build DLSS worker process" OFF)

if(NOT DEFINED NUKE_INSTALL_DIR)
    if(WIN32)
        set(NUKE_INSTALL_DIR "C:/Program Files/Nuke15.0v2")
    else()
        set(NUKE_INSTALL_DIR "/usr/local/Nuke17.0v1")
    endif()
endif()

if(NOT DEFINED NUKE_INCLUDE_DIR)
    set(NUKE_INCLUDE_DIR "${NUKE_INSTALL_DIR}/include")
endif()

if(NOT DEFINED NUKE_LIB_DIR)
    set(NUKE_LIB_DIR "${NUKE_INSTALL_DIR}")
endif()

if(NOT EXISTS "${NUKE_INCLUDE_DIR}/DDImage/Iop.h")
    message(FATAL_ERROR
        "Nuke NDK headers not found at ${NUKE_INCLUDE_DIR}/DDImage/Iop.h\n"
        "Pass -DNUKE_INSTALL_DIR=/path/to/Nuke17.0v6 (the folder containing include/ and libDDImage.so).")
endif()

add_library(DLSS5Live SHARED
    src/DLSS5Live.cpp
    src/DLSS5Live.h
    src/Platform.cpp
    src/Platform.h
    src/WorkerBridge.h
    src/WorkerBridge.cpp          # guarded: Windows only
    src/WorkerBridge_posix.cpp    # guarded: POSIX only
)

set_target_properties(DLSS5Live PROPERTIES PREFIX "")
if(WIN32)
    set_target_properties(DLSS5Live PROPERTIES SUFFIX ".dll")
else()
    # Nuke loads plug-ins as <Class>.so from its plug-in path.
    set_target_properties(DLSS5Live PROPERTIES SUFFIX ".so")
endif()

target_include_directories(DLSS5Live PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${NUKE_INCLUDE_DIR}"
)

target_compile_definitions(DLSS5Live PRIVATE
    DLSS5_VERSION_STRING="v${PROJECT_VERSION}"
)

if(WIN32)
    target_compile_definitions(DLSS5Live PRIVATE
        WIN32
        _WINDOWS
        NOMINMAX
        _USE_MATH_DEFINES
        _CRT_SECURE_NO_WARNINGS
        _CRT_NONSTDC_NO_WARNINGS
        NDEBUG
        _USRDLL
        _EXPORT=__declspec(dllexport)
    )
    target_link_directories(DLSS5Live PRIVATE "${NUKE_LIB_DIR}")
    target_link_libraries(DLSS5Live PRIVATE DDImage)
    if(MSVC)
        target_compile_options(DLSS5Live PRIVATE /wd4251 /wd4275 /MD /Zi)
        target_link_options(DLSS5Live PRIVATE /DEBUG)
    endif()
else()
    # ---- Linux ---------------------------------------------------------------
    # Nuke 13 and newer are built against the VFX Reference Platform toolchain
    # and use the C++11 std::string ABI. If you get a wall of
    # "undefined reference to ... std::__cxx11::basic_string ...", flip this to 0.
    if(NOT DEFINED NUKE_CXX11_ABI)
        set(NUKE_CXX11_ABI 1)
    endif()

    target_compile_definitions(DLSS5Live PRIVATE
        _GLIBCXX_USE_CXX11_ABI=${NUKE_CXX11_ABI}
        NDEBUG
        _EXPORT=
    )
    target_compile_options(DLSS5Live PRIVATE
        -fPIC -O3 -fvisibility=hidden -fvisibility-inlines-hidden
        -Wall -Wno-unused-parameter -g
    )
    # DDImage pulls in the rest of Nuke's libraries at runtime; do not demand
    # that every symbol resolve at link time.
    target_link_directories(DLSS5Live PRIVATE "${NUKE_LIB_DIR}")
    target_link_libraries(DLSS5Live PRIVATE DDImage pthread dl)
    target_link_options(DLSS5Live PRIVATE "-Wl,-rpath,${NUKE_LIB_DIR}")
endif()

if(BUILD_DLSS_WORKER)
    add_subdirectory(worker)
endif()
