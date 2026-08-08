cmake_minimum_required(VERSION 3.20)

project(fmd-router LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR
        "fmd-router is built on WASAPI and the Windows audio session APIs, "
        "so it is Windows-only by construction.")
endif()

# The filter is not vendored -- it is the same FmdDsp.hpp the VCV plugin builds,
# read straight from that checkout, so the two can never drift apart. The Rack
# stand-ins in fmd-vcv/tests are used for the same reason.
set(FMD_VCV_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../fmd-vcv"
    CACHE PATH "Path to the fmd-vcv checkout that supplies FmdDsp.hpp")

if(NOT EXISTS "${FMD_VCV_DIR}/src/FmdDsp.hpp")
    message(FATAL_ERROR
        "FmdDsp.hpp not found under FMD_VCV_DIR (${FMD_VCV_DIR}).\n"
        "Point it at the fmd-vcv checkout, e.g. -DFMD_VCV_DIR=D:/project/se_framework/fmd-vcv")
endif()
if(NOT EXISTS "${FMD_VCV_DIR}/tests/fmd_test_shim.hpp")
    message(FATAL_ERROR
        "fmd_test_shim.hpp not found under ${FMD_VCV_DIR}/tests -- it supplies the four "
        "Rack symbols FmdDsp.hpp needs to build outside Rack.")
endif()

add_executable(fmd-router WIN32
    src/Main.cpp
    src/Engine.cpp
    src/Devices.cpp
)

target_include_directories(fmd-router PRIVATE
    src
    "${FMD_VCV_DIR}/src"
    "${FMD_VCV_DIR}/tests"
)

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


# Offline checks for the chain, the ring and the resampler. No Windows APIs, so
# this target is plain portable C++ and runs anywhere the compiler does.
add_executable(fmd-router-tests tests/TestChain.cpp)

target_include_directories(fmd-router-tests PRIVATE
    src
    "${FMD_VCV_DIR}/src"
    "${FMD_VCV_DIR}/tests"
)

if(MSVC)
    target_compile_options(fmd-router-tests PRIVATE /W4 /permissive- /EHsc /utf-8)
else()
    target_compile_options(fmd-router-tests PRIVATE -Wall -Wextra)
endif()

enable_testing()
add_test(NAME chain COMMAND fmd-router-tests)
