cmake_minimum_required(VERSION 3.20)

project(wifi-warning VERSION 1.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0600)

if(MINGW)
    add_compile_options(-ffunction-sections -fdata-sections -Os)
    add_link_options(-Wl,--gc-sections)
endif()

if(MSVC)
    add_compile_options(/utf-8)
    add_compile_definitions(NOMINMAX)
    set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} /c65001")
endif()

add_library(ww_core STATIC
    src/core/auto_start.cpp
    src/core/config_manager.cpp
    src/core/http_probe.cpp
    src/core/json.cpp
    src/core/logger.cpp
    src/core/network_manager.cpp
    src/core/rule_engine.cpp
    src/core/shortcut_manager.cpp
    src/core/util.cpp
    src/core/wifi_detector.cpp
    src/core/wifi_switcher.cpp
    src/ui/browser_launcher.cpp
)

target_include_directories(ww_core PUBLIC src)
target_link_libraries(ww_core PUBLIC
    advapi32
    bcrypt
    comdlg32
    gdi32
    iphlpapi
    psapi
    ole32
    shell32
    uuid
    version
    wlanapi
    ws2_32
)

add_executable(wifi-warning WIN32
    src/main.cpp
    src/ui/api_handlers.cpp
    src/ui/http_server.cpp
    src/ui/tray_icon.cpp
    src/resources/wifi-warning.rc
)
target_link_libraries(wifi-warning PRIVATE ww_core)

add_executable(ww-launch WIN32
    src/ww-launch/main.cpp
    src/resources/ww-launch.rc
)
target_link_libraries(ww-launch PRIVATE iphlpapi shell32 wlanapi ws2_32)

add_executable(ww-smoke
    tests/smoke.cpp
    src/ui/api_handlers.cpp
    src/ui/http_server.cpp
)
target_link_libraries(ww-smoke PRIVATE ww_core)

if(MINGW)
    target_link_options(wifi-warning PRIVATE -municode -s)
    target_link_options(ww-launch PRIVATE -municode -s)
    target_link_options(ww-smoke PRIVATE -municode)
endif()

install(TARGETS wifi-warning ww-launch RUNTIME DESTINATION .)
install(DIRECTORY frontend DESTINATION .)
install(FILES config.json DESTINATION .)
install(FILES README.md DESTINATION . OPTIONAL)
install(FILES LICENSE DESTINATION . OPTIONAL)
install(FILES
    scripts/runtime-smoke.ps1
    scripts/tray-smoke.ps1
    scripts/browser-smoke.ps1
    scripts/browser-smoke-runner.js
    scripts/local-acceptance.ps1
    scripts/external-acceptance.ps1
    scripts/real-shortcut-scan-smoke.ps1
    scripts/elevated-wifi-acceptance.ps1
    scripts/package-portable.ps1
    DESTINATION scripts
    OPTIONAL
)
