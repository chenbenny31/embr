cmake_minimum_required(VERSION 3.25)
project(embr LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(URING REQUIRED liburing)


include(FetchContent)

FetchContent_Declare(httplib
        GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
        GIT_TAG v0.16.0
)
FetchContent_MakeAvailable(httplib)

FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
)
FetchContent_MakeAvailable(googletest)

# Transport lib
add_library(transport
        src/transport/tcp_server.cpp
        src/transport/tcp_client.cpp
        src/transport/tcp_transport.cpp
        src/transport/udp_transport.cpp
        src/transport/udp_data_client.cpp
        src/transport/udp_data_server.cpp
        src/util/io_uring_ctx.cpp
)
target_include_directories(transport PUBLIC src)
target_include_directories(transport PUBLIC ${URING_INCLUDE_DIRS})
target_link_libraries(transport PUBLIC ${URING_LIBRARIES})

# Core lib
add_library(core
        src/core/protocol.cpp
        src/core/push.cpp
        src/core/pull.cpp
        src/core/hash.cpp
        src/core/chunk_manager.cpp
        src/core/partial_file.cpp
)
target_link_libraries(core PUBLIC transport OpenSSL::Crypto)
target_include_directories(core PUBLIC src)

# Tracker lib
add_library(tracker
        src/tracker/tracker_client.cpp
        src/tracker/token_store.cpp
        src/tracker/tracker_handlers.cpp
        src/tracker/tracker_server.cpp
)
target_link_libraries(tracker PUBLIC core httplib::httplib OpenSSL::SSL OpenSSL::Crypto)
target_include_directories(tracker PUBLIC src)

# CLI lib
add_library(cli
        src/cli/push_cli.cpp
        src/cli/pull_cli.cpp
        src/cli/tracker_cli.cpp
        src/cli/trust_cli.cpp
)
target_link_libraries(cli PUBLIC core tracker)
target_include_directories(cli PUBLIC src)

# Main bin
add_executable(embr src/main.cpp)
target_link_libraries(embr PRIVATE cli)

# Test
add_executable(embr_test
        tests/test_protocol.cpp
        tests/test_tcp.cpp
        tests/test_udp.cpp
        tests/test_resume.cpp
        tests/test_tracker.cpp
)
target_link_libraries(embr_test PRIVATE core tracker GTest::gtest_main Threads::Threads)
include(GoogleTest)
gtest_discover_tests(embr_test)