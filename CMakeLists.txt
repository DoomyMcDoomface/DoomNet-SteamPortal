cmake_minimum_required(VERSION 3.20)

project(DoomNet LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Vendored Lua 5.4.7 and lua-cjson live in third_party/ and are built here.
# Nothing is looked up on the machine and nothing is downloaded. Local changes to Lua are
# listed in third_party/lua/DOOMNET-PATCHES.md.
#
# linit.c and liolib.c are left out on purpose. The host opens the standard
# libraries one by one (src/state_thread.cpp) and supplies its own `io` that
# opens files by wide path (src/host_fs.cpp). Nothing in the host calls the
# C runtime's fopen on a path from Lua.
set(DOOMNET_LUA_SOURCES
    third_party/lua/lapi.c
    third_party/lua/lauxlib.c
    third_party/lua/lbaselib.c
    third_party/lua/lcode.c
    third_party/lua/lcorolib.c
    third_party/lua/lctype.c
    third_party/lua/ldblib.c
    third_party/lua/ldebug.c
    third_party/lua/ldo.c
    third_party/lua/ldump.c
    third_party/lua/lfunc.c
    third_party/lua/lgc.c
    third_party/lua/llex.c
    third_party/lua/lmathlib.c
    third_party/lua/lmem.c
    third_party/lua/loadlib.c
    third_party/lua/lobject.c
    third_party/lua/lopcodes.c
    third_party/lua/loslib.c
    third_party/lua/lparser.c
    third_party/lua/lstate.c
    third_party/lua/lstring.c
    third_party/lua/lstrlib.c
    third_party/lua/ltable.c
    third_party/lua/ltablib.c
    third_party/lua/ltm.c
    third_party/lua/lundump.c
    third_party/lua/lutf8lib.c
    third_party/lua/lvm.c
    third_party/lua/lzio.c
)
# Lua is compiled as C++ so that lua_error and lua_yield throw real C++
# exceptions. Built as C, they longjmp, and on x64 MSVC that unwind through
# optimised C++ frames (the shims) failed the stack-cookie check and crashed
# (see third_party/lua/DOOMNET-PATCHES.md). /EHs, not /EHsc, is required so that
# calls into the extern "C" Lua API are allowed to throw.
set_source_files_properties(${DOOMNET_LUA_SOURCES} PROPERTIES LANGUAGE CXX)
if(MSVC)
    string(REPLACE "/EHsc" "/EHs" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    if(NOT CMAKE_CXX_FLAGS MATCHES "/EHs")
        string(APPEND CMAKE_CXX_FLAGS " /EHs")
    endif()
endif()
add_library(doomnet_lua STATIC ${DOOMNET_LUA_SOURCES})
target_include_directories(doomnet_lua PUBLIC third_party/lua)
target_compile_definitions(doomnet_lua PRIVATE
    DOOMNET_LUAJIT_NUMBER_FORMAT
    _CRT_SECURE_NO_WARNINGS
)

# Millennium's own single-file cjson fork, so json.encode / json.decode produce
# the same bytes the player's saved files were written with.
# See third_party/millennium-json/DOOMNET-PATCHES.md.
add_library(doomnet_cjson STATIC third_party/millennium-json/json.c)
target_link_libraries(doomnet_cjson PUBLIC doomnet_lua)
target_compile_definitions(doomnet_cjson PRIVATE
    _CRT_SECURE_NO_WARNINGS
    # A static library inside an .exe must not add an export table.
    CJSON_EXPORT=extern
)
if(MSVC)
    # Third-party C code gets the safety flags but not /WX.
    foreach(t doomnet_lua doomnet_cjson)
        target_compile_options(${t} PRIVATE /W3 /guard:cf /utf-8)
    endforeach()
endif()

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
    src/host_io.cpp
    src/process_job.cpp
    src/text.cpp
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
    src/resident.cpp
    src/global_hotkeys.cpp
    src/json_wire.cpp
    src/cdp_socket.cpp
    src/page_bridge.cpp
    src/cdp_pipe.cpp
    src/pipe_session.cpp
    src/skin_page.cpp
    src/art_thumbs.cpp
)

set(DOOMNET_BOOT_INC "${CMAKE_CURRENT_BINARY_DIR}/generated/boot_script.inc")
add_custom_command(
    OUTPUT "${DOOMNET_BOOT_INC}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/src/boot_script.js -DOUTPUT=${DOOMNET_BOOT_INC} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_boot.cmake
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/boot_script.js" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_boot.cmake"
    VERBATIM
)
add_custom_target(doomnet_boot DEPENDS "${DOOMNET_BOOT_INC}")
set_source_files_properties(src/page_bridge.cpp PROPERTIES OBJECT_DEPENDS "${DOOMNET_BOOT_INC}")

# src/boot_script.js is injected as text by the page bridge. It is not a compiled source.
add_executable(DoomNet WIN32 src/main.cpp ${DOOMNET_CORE})
add_executable(DoomNetFoundationTests src/foundation_test.cpp ${DOOMNET_CORE})
add_executable(DoomNetHostTests src/host_test.cpp ${DOOMNET_CORE})
add_executable(DoomNetLiveProof src/live_proof.cpp ${DOOMNET_CORE})
add_executable(DoomNetDummyChild WIN32 src/dummy_child.cpp)
add_executable(DoomNetWizardMock WIN32 src/wizard_mock.cpp)

# MinHook is the small x64 detour library the bootstrap uses to hook
# CreateProcessInternalW inside steam.exe. It is not used by DoomNet.exe.
add_library(doomnet_minhook STATIC
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde32.c
    third_party/minhook/src/hde/hde64.c
)
target_include_directories(doomnet_minhook PUBLIC third_party/minhook/include PRIVATE third_party/minhook/src)
if(MSVC)
    target_compile_options(doomnet_minhook PRIVATE /W3 /guard:cf)
    target_compile_definitions(doomnet_minhook PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# wsock32.dll sits beside steam.exe. export.wsock32.def forwards the real
# socket exports. src/bootstrap/wsock32.cpp is the hook and the pipe relay.
add_library(doomnet_wsock32 SHARED src/bootstrap/wsock32.cpp)
target_link_libraries(doomnet_wsock32 PRIVATE doomnet_minhook)
set_target_properties(doomnet_wsock32 PROPERTIES OUTPUT_NAME "wsock32")
if(MSVC)
    target_compile_options(doomnet_wsock32 PRIVATE /W4 /WX /utf-8 /EHsc /guard:cf)
    target_link_options(doomnet_wsock32 PRIVATE /GUARD:CF /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA)
endif()

add_library(doomnet_fake_steamui SHARED src/bootstrap/fake_steamui.cpp)
set_target_properties(doomnet_fake_steamui PROPERTIES OUTPUT_NAME "steamui")
if(MSVC)
    target_compile_options(doomnet_fake_steamui PRIVATE /W4 /WX /utf-8)
endif()

add_executable(DoomNetFakeSteam src/bootstrap/fake_steam.cpp)
target_link_libraries(DoomNetFakeSteam PRIVATE wsock32)
add_executable(DoomNetFakeWebHelper src/bootstrap/fake_webhelper.cpp)
add_executable(DoomNetWsockTests src/bootstrap/wsock_test.cpp)
if(MSVC)
    foreach(fake DoomNetFakeSteam DoomNetFakeWebHelper DoomNetWsockTests)
        target_compile_definitions(${fake} PRIVATE UNICODE _UNICODE)
        target_compile_options(${fake} PRIVATE /W4 /WX /utf-8 /EHsc)
    endforeach()
    set_target_properties(DoomNetFakeSteam DoomNetFakeWebHelper PROPERTIES LINK_FLAGS "/ENTRY:wmainCRTStartup")
    # Bind WSAStartup to wsock32.dll, not ws2_32, so the loader pulls in the
    # bootstrap that sits beside steam.exe.
    target_link_options(DoomNetFakeSteam PRIVATE "/NODEFAULTLIB:ws2_32.lib")
endif()

function(doomnet_apply target)
    target_include_directories(${target} PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_compile_definitions(${target} PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
    target_link_libraries(${target} PRIVATE advapi32 ole32 oleaut32 uuid ws2_32 winhttp bcrypt windowscodecs)
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
doomnet_apply(DoomNetHostTests)
doomnet_apply(DoomNetLiveProof)
add_dependencies(DoomNet doomnet_boot)
add_dependencies(DoomNetFoundationTests doomnet_boot)
add_dependencies(DoomNetFoundationTests doomnet_wsock32)
add_dependencies(DoomNetFoundationTests DoomNetFakeSteam)
add_dependencies(DoomNetFoundationTests DoomNetFakeWebHelper)
add_dependencies(DoomNetFoundationTests doomnet_fake_steamui)
add_dependencies(DoomNetHostTests doomnet_boot)
target_link_libraries(DoomNet PRIVATE doomnet_lua doomnet_cjson)
target_link_libraries(DoomNetFoundationTests PRIVATE doomnet_lua doomnet_cjson)
target_link_libraries(DoomNetHostTests PRIVATE doomnet_lua doomnet_cjson dbghelp)
target_link_libraries(DoomNetLiveProof PRIVATE doomnet_lua doomnet_cjson)
add_dependencies(DoomNetLiveProof doomnet_boot)
add_dependencies(DoomNetLiveProof doomnet_wsock32)
doomnet_apply(DoomNetDummyChild)
doomnet_apply(DoomNetWizardMock)
