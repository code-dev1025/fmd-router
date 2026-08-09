cmake_minimum_required(VERSION 3.20)

project(fmd-router LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR
        "The Real Mono Sound host is built on WASAPI and the Windows audio session "
        "APIs, so it is Windows-only by construction. The DSP itself is portable and "
        "is exercised by the fmd-router-tests target.")
endif()

# The chain has no third-party dependency: the Hilbert transformer, the
# quadrature network, the filters and the limiter are all in src/RealMono.h.
# See the header for the RS-MET classes each one stands in for.

add_executable(fmd-router WIN32
    src/Main.cpp
    src/Engine.cpp
    src/Devices.cpp
)

target_include_directories(fmd-router PRIVATE src)

target_compile_definitions(fmd-router PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX          # or windows.h's min/max macros break std::min/std::max
)

target_link_libraries(fmd-router PRIVATE
    ole32       # COM, and the device enumerator
    avrt        # MMCSS, for Pro Audio thread priority
    comctl32    # trackbars
)

if(MSVC)
    target_compile_options(fmd-router PRIVATE /W4 /permissive- /EHsc /utf-8)
    # Windows subsystem with a wide entry point.
    target_link_options(fmd-router PRIVATE /ENTRY:wWinMainCRTStartup)
else()
    target_compile_options(fmd-router PRIVATE -Wall -Wextra)
endif()

set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT fmd-router)


# Offline checks for the Real Mono chain, the ring and the resampler. No
# Windows APIs, so this target is plain portable C++ and runs anywhere the
# compiler does.
add_executable(fmd-router-tests tests/TestRealMono.cpp)

target_include_directories(fmd-router-tests PRIVATE src)

if(MSVC)
    target_compile_options(fmd-router-tests PRIVATE /W4 /permissive- /EHsc /utf-8)
else()
    target_compile_options(fmd-router-tests PRIVATE -Wall -Wextra)
endif()

enable_testing()
add_test(NAME realmono COMMAND fmd-router-tests)
