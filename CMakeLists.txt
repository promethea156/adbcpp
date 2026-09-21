cmake_minimum_required(VERSION 3.24)

project(
  adbcpp
  VERSION 2.1.0
  DESCRIPTION "A simplified, self-contained ADB client library in C++"
  HOMEPAGE_URL "https://github.com/promethea156/adbcpp"
  LANGUAGES CXX
)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(ADBCPP_IS_TOP_LEVEL ON)
else()
  set(ADBCPP_IS_TOP_LEVEL OFF)
endif()

option(ADBCPP_BUILD_TESTS "Build adbcpp tests" ${ADBCPP_IS_TOP_LEVEL})
option(ADBCPP_BUILD_EXAMPLES "Build adbcpp examples" ${ADBCPP_IS_TOP_LEVEL})
option(ADBCPP_BUILD_DOCS "Build adbcpp documentation" OFF)
option(ADBCPP_BUILD_USB "Build the adbcpp USB backend (libusb)" ${ADBCPP_IS_TOP_LEVEL})
option(ADBCPP_INSTALL "Generate adbcpp install rules" ${ADBCPP_IS_TOP_LEVEL})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(GNUInstallDirs)

# `tl::expected` provides the `Result<T>` that every fallible operation returns
# (see `docs/07-error-model.md`). It is header-only and licensed CC0-1.0, so it
# carries no license obligations, but it does have to be told not to build its own
# tests or install rules: its tests fetch their own Catch2 v2, which would collide
# with the v3 this project uses, and its install rules would install a second copy
# of a dependency. Setting the options before `FetchContent_MakeAvailable` is the
# same trap as `LIBUSB_ENABLE_UDEV` and `MBEDTLS_FATAL_WARNINGS` (blockers 20
# and 22).
include(FetchContent)
set(EXPECTED_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(EXPECTED_BUILD_PACKAGE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  tl_expected
  GIT_REPOSITORY https://github.com/TartanLlama/expected.git
  GIT_TAG v1.3.1
)
FetchContent_MakeAvailable(tl_expected)

add_library(adbcpp
  src/app.cpp
  src/connection.cpp
  src/log.cpp
  src/protocol/message.cpp
  src/session.cpp
  src/shell.cpp
  src/stream.cpp
  src/sync.cpp
  src/tcp/tcp_transport.cpp
  src/transport.cpp
)
add_library(adbcpp::adbcpp ALIAS adbcpp)

target_compile_features(adbcpp PUBLIC cxx_std_20)

target_include_directories(adbcpp PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

# `<tl/expected.hpp>` appears in the public `error.hpp`, so `tl::expected` is a
# public dependency of the core target, which is no longer dependency-free.
target_link_libraries(adbcpp PUBLIC tl::expected)

# `TcpTransport` uses the platform's sockets, so on Windows the core links the
# Winsock library. It is a system library with no license obligations, so the core
# stays free of any third-party dependency.
if(WIN32)
  target_link_libraries(adbcpp PRIVATE ws2_32)
endif()

set_target_properties(adbcpp PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)

if(BUILD_SHARED_LIBS)
  target_compile_definitions(adbcpp
    PRIVATE ADBCPP_EXPORTS
    INTERFACE ADBCPP_SHARED
  )
endif()

if(MSVC)
  target_compile_options(adbcpp PRIVATE /W4 /permissive-)
else()
  target_compile_options(adbcpp PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(ADBCPP_BUILD_USB)
  add_subdirectory(src/usb)
endif()

add_subdirectory(src/crypto)

if(ADBCPP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(ADBCPP_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(ADBCPP_BUILD_DOCS)
  add_subdirectory(docs)
endif()

if(ADBCPP_IS_TOP_LEVEL)
  include(cmake/Format.cmake)
endif()

if(ADBCPP_INSTALL)
  include(CMakePackageConfigHelpers)

  install(TARGETS adbcpp
    EXPORT adbcppTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  )

  install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

  # The crypto and USB backends are exported as `adbcpp::crypto` and
  # `adbcpp::usb`, so a `find_package(adbcpp)` consumer gets the same three
  # targets as a FetchContent consumer. They are static libraries with private
  # third-party dependencies (mbedTLS, libusb), and a static library propagates
  # its private dependencies to the final link, so those dependencies have to be in
  # the export set too. They are installed next to `adbcpp` and are only used
  # internally; `adbcpp`'s own public headers do not include them.
  install(TARGETS adbcpp-crypto mbedcrypto everest p256m
    EXPORT adbcppTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  )

  # `tl::expected` is header-only, and `EXPECTED_BUILD_PACKAGE OFF` keeps it
  # from installing itself, so its target and its headers are installed here. It is
  # exported under the project's own namespace, like mbedcrypto and usb-1.0 above.
  install(TARGETS expected
    EXPORT adbcppTargets
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  )
  install(DIRECTORY "${tl_expected_SOURCE_DIR}/include/" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  if(ADBCPP_BUILD_USB)
    install(TARGETS adbcpp-usb usb-1.0
      EXPORT adbcppTargets
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
      INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
  endif()

  install(EXPORT adbcppTargets
    FILE adbcppTargets.cmake
    NAMESPACE adbcpp::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/adbcpp
  )

  configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/adbcppConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/adbcppConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/adbcpp
  )

  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/adbcppConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
  )

  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/adbcppConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/adbcppConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/adbcpp
  )
endif()
