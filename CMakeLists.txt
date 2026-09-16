cmake_minimum_required(VERSION 3.24)

project(
  adbcpp
  VERSION 0.1.0
  DESCRIPTION "A simplified, dependency-free ADB client library in C++"
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
option(ADBCPP_INSTALL "Generate adbcpp install rules" ${ADBCPP_IS_TOP_LEVEL})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(GNUInstallDirs)

add_library(adbcpp
  src/protocol/message.cpp
  src/session.cpp
)
add_library(adbcpp::adbcpp ALIAS adbcpp)

target_compile_features(adbcpp PUBLIC cxx_std_20)

target_include_directories(adbcpp PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

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
