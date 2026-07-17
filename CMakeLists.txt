cmake_minimum_required(VERSION 3.25)
project(PowerScope VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(PowerScopeCore
    src/dashboard.cpp
    src/diagnostics.cpp
    src/monitoring_session.cpp
)
target_include_directories(PowerScopeCore PUBLIC src)

add_executable(PowerScope
    src/main.cpp
    src/terminal_renderer.cpp
    src/windows_probes.cpp
)
target_link_libraries(PowerScope PRIVATE PowerScopeCore)

target_compile_definitions(PowerScope PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NTDDI_VERSION=0x0A000000
)

target_link_libraries(PowerScope PRIVATE
    PowrProf
    Iphlpapi
    Wbemuuid
    Ole32
    OleAut32
)

if(MSVC)
    target_compile_options(PowerScope PRIVATE /W4 /permissive- /utf-8 /EHsc)
    target_compile_options(PowerScopeCore PRIVATE /W4 /permissive- /utf-8 /EHsc)
    set_property(TARGET PowerScope PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET PowerScopeCore PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(PowerScope PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(PowerScopeCore PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(PowerScopeTests tests/powerscope_tests.cpp)
    target_link_libraries(PowerScopeTests PRIVATE PowerScopeCore)
    if(MSVC)
        target_compile_options(PowerScopeTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
        set_property(TARGET PowerScopeTests PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        target_compile_options(PowerScopeTests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME PowerScopeTests COMMAND PowerScopeTests)
endif()
