cmake_minimum_required(VERSION 3.25)
project(PowerScope VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(PowerScope src/main.cpp)

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
    set_property(TARGET PowerScope PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(PowerScope PRIVATE -Wall -Wextra -Wpedantic)
endif()
