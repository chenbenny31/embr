cmake_minimum_required(VERSION 3.25)
project(embr)

set(CMAKE_CXX_STANDARD 20)

# transport lib
add_library(embr_lib
        src/transport/tcp_server.cpp
        src/transport/tcp_client.cpp
)
target_include_directories(embr_lib PUBLIC src)

add_executable(embr src/main.cpp)
target_link_libraries(embr embr_lib)

include(FetchContent)
FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.17.0
)
FetchContent_MakeAvailable(googletest)

enable_testing()
add_subdirectory(tests)