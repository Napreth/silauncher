cmake_minimum_required(VERSION 3.20)

project(silauncher
    VERSION 0.1.0
    DESCRIPTION "Silent launcher for Windows GUI applications"
    LANGUAGES CXX
)

if(NOT WIN32)
    message(FATAL_ERROR "silauncher targets Windows only")
endif()

add_executable(silauncher WIN32
    src/main.cpp
)

target_compile_features(silauncher PRIVATE cxx_std_20)
target_compile_definitions(silauncher PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    target_compile_options(silauncher PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(silauncher PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(MINGW)
    target_link_options(silauncher PRIVATE -municode)
endif()

target_link_libraries(silauncher PRIVATE shell32 user32)

install(TARGETS silauncher RUNTIME DESTINATION .)
