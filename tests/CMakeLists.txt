include(FetchContent)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.7.1
)
FetchContent_MakeAvailable(Catch2)

add_executable(adbcpp_tests
  protocol_message_test.cpp
  session_test.cpp
)
target_link_libraries(adbcpp_tests PRIVATE adbcpp::adbcpp Catch2::Catch2WithMain)

list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)
catch_discover_tests(adbcpp_tests)
