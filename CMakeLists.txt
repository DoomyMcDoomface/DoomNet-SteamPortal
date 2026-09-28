cmake_minimum_required(VERSION 3.20)

project(DoomNet LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(DOOMNET_CORE
    src/log.cpp
    src/paths.cpp
    src/instance.cpp
    src/config.cpp
    src/steam_discover.cpp
    src/state_thread.cpp
    src/browser_thread.cpp
    src/steam_attach.cpp
    src/stream.cpp
    src/host_fs.cpp
    src/host_json.cpp
    src/host_logger.cpp
    src/host_utils.cpp
    src/host_millennium.cpp
    src/toggle.cpp
    src/installer.cpp
    src/wide_io.cpp
    src/launch.cpp
    src/shortcut.cpp
    src/payload.cpp
)

# src/boot_script.js is a comment guide. It is not compiled and not injected.
add_executable(DoomNet WIN32 src/main.cpp ${DOOMNET_CORE})
add_executable(DoomNetFoundationTests src/foundation_test.cpp ${DOOMNET_CORE})
add_executable(DoomNetDummyChild WIN32 src/dummy_child.cpp)
add_executable(DoomNetWizardMock WIN32 src/wizard_mock.cpp)

function(doomnet_apply target)
    target_include_directories(${target} PRIVATE src)
    target_compile_definitions(${target} PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
    target_link_libraries(${target} PRIVATE advapi32 ole32 uuid)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /WX
            /permissive-
            /utf-8
            /guard:cf
        )
        target_link_options(${target} PRIVATE
            /GUARD:CF
            /DYNAMICBASE
            /NXCOMPAT
            /HIGHENTROPYVA
        )
    endif()
endfunction()

doomnet_apply(DoomNet)
doomnet_apply(DoomNetFoundationTests)
doomnet_apply(DoomNetDummyChild)
doomnet_apply(DoomNetWizardMock)
