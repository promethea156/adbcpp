include(FetchContent)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.7.1
)
FetchContent_MakeAvailable(Catch2)

add_executable(adbcpp_tests
  adb_key_test.cpp
  app_test.cpp
  connection_test.cpp
  protocol_message_test.cpp
  session_test.cpp
  shell_test.cpp
  stream_test.cpp
  sync_test.cpp
  tcp_test.cpp
)
target_link_libraries(adbcpp_tests
  PRIVATE adbcpp::adbcpp adbcpp::crypto MbedTLS::mbedcrypto
    Catch2::Catch2WithMain
)

list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)
catch_discover_tests(adbcpp_tests)

if(ADBCPP_BUILD_USB)
  add_executable(adbcpp_device_tests device_test.cpp)
  target_link_libraries(adbcpp_device_tests
    PRIVATE adbcpp::usb adbcpp::crypto
  )

  # See examples/CMakeLists.txt: the copy is Windows-only because
  # `$<TARGET_RUNTIME_DLLS:...>` is empty elsewhere (blocker 21).
  if(WIN32)
    add_custom_command(TARGET adbcpp_device_tests POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_RUNTIME_DLLS:adbcpp_device_tests>
              $<TARGET_FILE_DIR:adbcpp_device_tests>
      COMMAND_EXPAND_LISTS
    )
  endif()

  add_test(NAME device COMMAND adbcpp_device_tests)
  set_tests_properties(device PROPERTIES SKIP_RETURN_CODE 77)

  # `DeviceId::parse` is the device-free part of the USB backend, so it runs
  # everywhere the backend is built and is not skipped without a device.
  add_executable(adbcpp_usb_tests usb_test.cpp)
  target_link_libraries(adbcpp_usb_tests
    PRIVATE adbcpp::usb Catch2::Catch2WithMain
  )
  if(WIN32)
    add_custom_command(TARGET adbcpp_usb_tests POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_RUNTIME_DLLS:adbcpp_usb_tests>
              $<TARGET_FILE_DIR:adbcpp_usb_tests>
      COMMAND_EXPAND_LISTS
    )
  endif()
  catch_discover_tests(adbcpp_usb_tests)
endif()
