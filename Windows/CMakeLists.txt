cmake_minimum_required(VERSION 3.20)
project(MaxWatch VERSION 1.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static MSVC runtime (/MT) so MaxWatch.exe runs out-of-the-box on every clean Windows 10 & Windows 11 PC
# without requiring any VC++ Redistributable download!
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(MaxWatch WIN32
    src/main.cpp
    src/AppWindow.cpp
    src/AppWindow.hpp
    src/VaultCrypto.cpp
    src/VaultCrypto.hpp
    src/Fmp4Muxer.cpp
    src/Fmp4Muxer.hpp
    src/TimestampStamper.cpp
    src/TimestampStamper.hpp
    src/SleepAndLockManager.cpp
    src/SleepAndLockManager.hpp
    src/MediaPipeline.cpp
    src/MediaPipeline.hpp
    src/SelfTest.cpp
    src/SelfTest.hpp
    src/resource.h
    resources/MaxWatch.rc
)

target_include_directories(MaxWatch PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(MaxWatch PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

if(MSVC)
    target_compile_options(MaxWatch PRIVATE
        /utf-8
        /W4
        /EHsc
        /MP
        $<$<CONFIG:Release>:/O2 /guard:cf /Gy /Gw>
    )
    target_link_options(MaxWatch PRIVATE
        /MANIFEST:NO
        /DYNAMICBASE
        /HIGHENTROPYVA
        /NXCOMPAT
        $<$<CONFIG:Release>:/GUARD:CF /OPT:REF /OPT:ICF>
    )
endif()

# Zero network libraries linked (ws2_32, wininet, winhttp, urlmon, dnsapi strictly absent!)
target_link_libraries(MaxWatch PRIVATE
    bcrypt
    mfplat
    mf
    mfreadwrite
    mfuuid
    wmcodecdspuuid
    strmiids
    d2d1
    dwrite
    windowscodecs
    dwmapi
    wtsapi32
    ole32
    oleaut32
    propsys
    uuid
    shell32
    shlwapi
    advapi32
    user32
    gdi32
    comctl32
)
