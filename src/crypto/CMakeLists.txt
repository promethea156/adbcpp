include(FetchContent)

set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  mbedtls
  GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
  GIT_TAG v3.6.2
)
FetchContent_MakeAvailable(mbedtls)

add_library(adbcpp-crypto adb_key.cpp)
add_library(adbcpp::crypto ALIAS adbcpp-crypto)

target_compile_features(adbcpp-crypto PUBLIC cxx_std_20)
target_compile_definitions(adbcpp-crypto PRIVATE ADBCPP_EXPORTS)
target_link_libraries(adbcpp-crypto
  PUBLIC adbcpp::adbcpp
  PRIVATE MbedTLS::mbedcrypto
)

if(MSVC)
  target_compile_options(adbcpp-crypto PRIVATE /W4 /permissive-)
else()
  target_compile_options(adbcpp-crypto PRIVATE -Wall -Wextra -Wpedantic)
endif()
