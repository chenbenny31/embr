add_executable(test
        tests/test_protocol.cpp
)
target_link_libraries(test PRIVATE core GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(test)