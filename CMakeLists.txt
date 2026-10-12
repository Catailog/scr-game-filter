cmake_minimum_required(VERSION 3.20)
project(scr-game-filter LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# VC++ 런타임을 exe에 정적으로 묶어서 런타임이 없는 PC에서도 실행되게 한다.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(scr-game-filter WIN32
    src/main.cpp
    src/room_proxy.cpp
    src/system_proxy.cpp
)

target_compile_definitions(scr-game-filter PRIVATE UNICODE _UNICODE)
target_link_libraries(scr-game-filter PRIVATE ws2_32 wininet shell32)

if (MSVC)
    target_compile_options(scr-game-filter PRIVATE /utf-8)
endif()
