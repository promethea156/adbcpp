cmake_minimum_required(VERSION 3.24)

project(
  adbcpp
  VERSION 2.2.1
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
option(ADBCPP_BUILD_COMPRESSION "Build adbcpp compression (zstd)" ${ADBCPP_IS_TOP_LEVEL})
option(ADBCPP_BUILD_LZ4 "Build the adbcpp lz4 codec (lz4)" ${ADBCPP_IS_TOP_LEVEL})
option(ADBCPP_BUILD_BROTLI "Build the adbcpp brotli codec (brotli)" ${ADBCPP_IS_TOP_LEVEL})
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

# zstd provides the compression that the v2 `RECV`/`SEND` forms use (see
# `docs/09-sendrecv-v2.md`). It is fetched only when compression is built, and
# its programs and tests are turned off before `FetchContent_MakeAvailable`, the
# same trap as `EXPECTED_BUILD_TESTS` above (blockers 20 and 22). It is
# BSD-3-Clause, so it carries no copyleft obligation.
if(ADBCPP_BUILD_COMPRESSION)
  set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
  set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    zstd
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG v1.5.7
    SOURCE_SUBDIR build/cmake
  )
  FetchContent_MakeAvailable(zstd)
endif()

# lz4 provides the second codec the v2 `RECV`/`SEND` forms can use (see
# `docs/09-sendrecv-v2.md`). It is fetched only when it is built, and its CLI is
# turned off before `FetchContent_MakeAvailable`, the same trap as
# `ZSTD_BUILD_PROGRAMS` above. It is BSD-2-Clause, so it carries no copyleft
# obligation.
if(ADBCPP_BUILD_LZ4)
  set(LZ4_BUILD_CLI OFF CACHE BOOL "" FORCE)
  set(LZ4_BUILD_LEGACY_LZ4C OFF CACHE BOOL "" FORCE)
  set(LZ4_BUILD_LEGACY_LZ4HC OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    lz4
    GIT_REPOSITORY https://github.com/lz4/lz4.git
    GIT_TAG v1.10.0
    SOURCE_SUBDIR build/cmake
  )
  FetchContent_MakeAvailable(lz4)
endif()

# brotli provides the third codec the v2 `RECV`/`SEND` forms can use (see
# `docs/09-sendrecv-v2.md`). It has no single-file build, so it brings many
# sources, and it is fetched only when it is built. Its tools and tests are turned
# off before `FetchContent_MakeAvailable`, the same trap as `ZSTD_BUILD_PROGRAMS`
# above. It is MIT, so it carries no copyleft obligation.
if(ADBCPP_BUILD_BROTLI)
  set(BROTLI_BUNDLED_MODE ON CACHE BOOL "" FORCE)
  set(BROTLI_DISABLE_TESTS ON CACHE BOOL "" FORCE)
  set(BROTLI_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  # Brotli declares `option(BUILD_SHARED_LIBS ... ON)` for itself, which would
  # otherwise turn this project's shared/static choice on. The caller's value is saved
  # and restored around the subdirectory, and brotli is built statically so it can be
  # linked privately like zstd and lz4.
  set(ADBCPP_BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS}")
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    brotli
    GIT_REPOSITORY https://github.com/google/brotli.git
    GIT_TAG v1.1.0
  )
  FetchContent_MakeAvailable(brotli)
  set(BUILD_SHARED_LIBS "${ADBCPP_BUILD_SHARED_LIBS}" CACHE BOOL "" FORCE)
endif()

# The v2 `RECV`/`SEND` forms are compiled in when any codec is built, and
# `ADBCPP_HAS_COMPRESSION` gates that wire-format code. Each codec has its own
# define so a build can turn one on without the others.
if(ADBCPP_BUILD_COMPRESSION OR ADBCPP_BUILD_LZ4 OR ADBCPP_BUILD_BROTLI)
  set(ADBCPP_HAVE_COMPRESSION TRUE)
endif()

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

# `src/compression.cpp` uses `<zstd.h>`, `<lz4frame.h>`, and the brotli headers,
# but those stay internal, so the codecs are private dependencies of the core and no
# codec type reaches the public API. `ADBCPP_HAS_COMPRESSION` gates the v2 wire
# format and is defined when any codec is built; each codec also has its own define,
# so a build can turn one on without the others and the banner only names the built
# ones.
if(ADBCPP_HAVE_COMPRESSION)
  target_sources(adbcpp PRIVATE src/compression.cpp)
  target_compile_definitions(adbcpp PRIVATE ADBCPP_HAS_COMPRESSION)
endif()
if(ADBCPP_BUILD_COMPRESSION)
  target_link_libraries(adbcpp PRIVATE libzstd_static)
  target_compile_definitions(adbcpp PRIVATE ADBCPP_HAS_ZSTD)
endif()
if(ADBCPP_BUILD_LZ4)
  target_link_libraries(adbcpp PRIVATE lz4_static)
  target_compile_definitions(adbcpp PRIVATE ADBCPP_HAS_LZ4)
endif()
if(ADBCPP_BUILD_BROTLI)
  target_link_libraries(adbcpp PRIVATE brotlienc brotlidec)
  target_compile_definitions(adbcpp PRIVATE ADBCPP_HAS_BROTLI)
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

  # zstd is a static library with no other dependency, and it is installed next
  # to `adbcpp` for the same reason mbedcrypto is: a static library propagates
  # its private dependencies to the final link, so zstd has to be in the export set.
  # Only its public headers are installed, not the whole source `lib/`.
  if(ADBCPP_BUILD_COMPRESSION)
    install(TARGETS libzstd_static
      EXPORT adbcppTargets
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
      INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    install(FILES
      "${zstd_SOURCE_DIR}/lib/zstd.h"
      "${zstd_SOURCE_DIR}/lib/zstd_errors.h"
      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
  endif()

  if(ADBCPP_BUILD_LZ4)
    install(TARGETS lz4_static
      EXPORT adbcppTargets
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
      INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    install(FILES
      "${lz4_SOURCE_DIR}/lib/lz4.h"
      "${lz4_SOURCE_DIR}/lib/lz4frame.h"
      "${lz4_SOURCE_DIR}/lib/lz4hc.h"
      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
  endif()

  # brotli is three static libraries with no other dependency, installed next
  # to `adbcpp` for the same reason zstd is: a static library propagates its
  # private dependencies to the final link, so they have to be in the export set.
  if(ADBCPP_BUILD_BROTLI)
    install(TARGETS brotlienc brotlidec brotlicommon
      EXPORT adbcppTargets
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
      INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    install(DIRECTORY "${brotli_SOURCE_DIR}/c/include/brotli"
      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
  endif()

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
