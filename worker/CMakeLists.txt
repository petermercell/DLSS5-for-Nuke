cmake_minimum_required(VERSION 3.16)
project(DLSS_Nuke_Worker CXX)

set(CMAKE_CXX_STANDARD 17)

# The worker is D3D12 + NGX, so the *target* is always Windows. It can be built
# either natively with MSVC or cross-compiled from Linux with mingw-w64 (see
# cmake/toolchain-mingw64.cmake) and then run under Wine/Proton.
if(NOT WIN32)
    message(FATAL_ERROR
        "The worker targets Windows (D3D12 + NGX).\n"
        "To cross-compile it from Linux, configure with:\n"
        "  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake")
endif()

# 1. Main Worker Executable
add_executable(DLSS_Nuke_Worker WIN32
    main.cpp
    DLSSWorker.cpp
    DISOpticalFlow.cpp
)

# Switch to CONSOLE subsystem for stdin/stdout communication with Nuke
set_target_properties(DLSS_Nuke_Worker PROPERTIES
    WIN32_EXECUTABLE FALSE
)

target_compile_definitions(DLSS_Nuke_Worker PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _UNICODE
    UNICODE
)

target_link_libraries(DLSS_Nuke_Worker PRIVATE
    d3d12
    dxgi
    dxguid
)

# 2. Native Caller Shim DLL (named nvngx.dll to satisfy NGX snippet return address check)
add_library(nvngx SHARED
    caller_shim.cpp
)

set_target_properties(nvngx PROPERTIES
    OUTPUT_NAME "nvngx"
    PREFIX ""
)

target_compile_definitions(nvngx PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(nvngx PRIVATE d3d12)

if(MSVC)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        target_compile_options(DLSS_Nuke_Worker PRIVATE /O2 /GL)
        target_link_options(DLSS_Nuke_Worker PRIVATE /LTCG)
        target_compile_options(nvngx PRIVATE /O2)
    endif()
else()
    # mingw-w64: statically link the GCC runtime so the produced PE has no
    # libgcc/libstdc++ DLL dependencies inside the Wine prefix.
    target_compile_options(DLSS_Nuke_Worker PRIVATE -O2)
    target_link_options(DLSS_Nuke_Worker PRIVATE -static -static-libgcc -static-libstdc++)

    # IMPORTANT: do NOT enable LTO here. The shim's whole purpose is to own the
    # return address the NR runtime validates; link-time optimisation across the
    # call boundary is exactly what would defeat it.
    target_compile_options(nvngx PRIVATE -O2)
    target_link_options(nvngx PRIVATE -static -static-libgcc -static-libstdc++)
endif()

install(TARGETS DLSS_Nuke_Worker nvngx
        RUNTIME DESTINATION .
        LIBRARY DESTINATION .)
