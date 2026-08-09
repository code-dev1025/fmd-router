cmake_minimum_required(VERSION 3.20)

project(fmd-router LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The chain has no third-party dependency: the Hilbert transformer, the
# quadrature network, the filters and the limiter are all in src/RealMono.h.
# See the header for the RS-MET classes each one stands in for.

# Only the host is Windows-only. The chain, its tests and the offline WAV tool
# are portable, and are worth being able to build and run anywhere -- so the
# platform check guards the app target rather than the whole project.
if(WIN32)
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

    set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                 PROPERTY VS_STARTUP_PROJECT fmd-router)
else()
    message(STATUS
        "Not Windows: building the chain, its tests and realmono-wav only. "
        "The capture/render host is WASAPI and does not apply here.")
endif()


# Offline WAV processing, for A/B against the client's before/after material
# without a virtual cable or a second endpoint. Same chain the audio thread
# runs, so what it writes is what the app would have played.
add_executable(realmono-wav tools/WavProcess.cpp)

target_include_directories(realmono-wav PRIVATE src)

if(MSVC)
    target_compile_options(realmono-wav PRIVATE /W4 /permissive- /EHsc /utf-8)
else()
    target_compile_options(realmono-wav PRIVATE -Wall -Wextra)
endif()


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
