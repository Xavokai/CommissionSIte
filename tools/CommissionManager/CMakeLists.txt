cmake_minimum_required(VERSION 3.16)
project(CommissionManager LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
option(COMMISSION_MANAGER_TESTS "Build the core_tests console program" ON)

set(WIN_LIBS comctl32 comdlg32 shell32 ole32 oleaut32 uuid windowscodecs uxtheme msimg32 gdi32 user32 advapi32)

function(apply_common target)
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /EHsc)
    else()
        target_compile_options(${target} PRIVATE -Wall -municode)
        target_link_options(${target} PRIVATE -municode -static)
    endif()
    target_link_libraries(${target} PRIVATE ${WIN_LIBS})
endfunction()

add_executable(CommissionManager WIN32 src/main.cpp src/core.cpp src/app.rc)
apply_common(CommissionManager)
if(MSVC)
    # The manifest (per-monitor v2 DPI, common controls v6) is embedded by app.rc.
    target_link_options(CommissionManager PRIVATE /MANIFEST:NO)
endif()
# Same location as build.bat: CommissionManager.exe next to this file.
add_custom_command(TARGET CommissionManager POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:CommissionManager> ${CMAKE_CURRENT_SOURCE_DIR}/CommissionManager.exe)

if(COMMISSION_MANAGER_TESTS)
    add_executable(core_tests tests/core_tests.cpp src/core.cpp)
    apply_common(core_tests)
endif()
