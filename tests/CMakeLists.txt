enable_testing()
find_package(GTest REQUIRED)
add_executable(embr_tests
        dummy_test.cpp
        echo_test.cpp
)
target_link_libraries(embr_tests GTest::gtest_main embr_lib)
add_test(NAME embr_tests COMMAND embr_tests)
