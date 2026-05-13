cmake_minimum_required(VERSION 3.20)

project(MiniNet LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(MiniNet
    src/MiniNet/src/ack_tracker.cpp
    src/MiniNet/src/connection.cpp
    src/MiniNet/src/packet.cpp
    src/MiniNet/src/ping.cpp
    src/MiniNet/src/udp_socket.cpp
)

target_include_directories(MiniNet
    PUBLIC
        src/MiniNet/include
)

if (WIN32)
    target_compile_definitions(MiniNet PUBLIC _WIN32_WINNT=0x0600)
    target_link_libraries(MiniNet PUBLIC ws2_32)
endif()

add_executable(MiniNet.Server
    src/MiniNet.Server/main.cpp
)
target_link_libraries(MiniNet.Server PRIVATE MiniNet)

add_executable(MiniNet.Client
    src/MiniNet.Client/main.cpp
)
target_link_libraries(MiniNet.Client PRIVATE MiniNet)

enable_testing()

add_executable(MiniNet.Tests
    tests/MiniNet.Tests/test_main.cpp
)
target_link_libraries(MiniNet.Tests PRIVATE MiniNet)

add_test(NAME MiniNet.Tests COMMAND MiniNet.Tests)
