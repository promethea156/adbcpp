add_executable(adbcpp_sample sample/main.cpp)
target_link_libraries(adbcpp_sample PRIVATE adbcpp::adbcpp)

add_executable(adbcpp_sync_example sync/main.cpp)
target_link_libraries(adbcpp_sync_example PRIVATE adbcpp::adbcpp)

# The TCP example needs a listening device, an emulator or a `tcpip` device, so it
# is not registered. It has no USB dependency, so it links only the core.
add_executable(adbcpp_tcp_example tcp/main.cpp)
target_link_libraries(adbcpp_tcp_example
  PRIVATE adbcpp::adbcpp adbcpp::crypto
)

# Drives two devices from one thread over loopback listeners that stand in for an
# emulator's ADB listener, so it needs no device and is device-free like the sync
# example. It has no USB dependency, so it links only the core.
add_executable(adbcpp_poll_example poll/main.cpp)
target_link_libraries(adbcpp_poll_example PRIVATE adbcpp::adbcpp)

# The sample, the sync example, and the poll example are device-free, so CTest
# runs them as part of the normal test suite. The USB example needs a device and is
# not registered.
if(ADBCPP_BUILD_TESTS)
  add_test(NAME example_sample COMMAND adbcpp_sample)
  add_test(NAME example_sync COMMAND adbcpp_sync_example)
  add_test(NAME example_poll COMMAND adbcpp_poll_example)
endif()

if(ADBCPP_BUILD_USB)
  add_executable(adbcpp_usb_example usb/main.cpp)
  target_link_libraries(adbcpp_usb_example
    PRIVATE adbcpp::usb adbcpp::crypto
  )

  # Drives two devices on two threads, so it needs a device too.
  add_executable(adbcpp_multi_example multi/main.cpp)
  target_link_libraries(adbcpp_multi_example
    PRIVATE adbcpp::usb adbcpp::crypto
  )

  # Walks every feature once against the first attached device, so it needs one.
  add_executable(adbcpp_demo_example demo/main.cpp)
  target_link_libraries(adbcpp_demo_example
    PRIVATE adbcpp::usb adbcpp::crypto
  )

  # Walks every feature once on every attached device, one thread each, so the
  # install, launch, and uninstall steps run concurrently. It needs a device.
  add_executable(adbcpp_demo_multi_example demo_multi/main.cpp)
  target_link_libraries(adbcpp_demo_multi_example
    PRIVATE adbcpp::usb adbcpp::crypto
  )

  # `$<TARGET_RUNTIME_DLLS:...>` is always empty on non-DLL platforms, and
  # `cmake -E copy_if_different` then fails because it is left with a single
  # argument, so the copy is Windows-only (blocker 21).
  if(WIN32)
    add_custom_command(TARGET adbcpp_usb_example POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_RUNTIME_DLLS:adbcpp_usb_example>
              $<TARGET_FILE_DIR:adbcpp_usb_example>
      COMMAND_EXPAND_LISTS
    )
    add_custom_command(TARGET adbcpp_multi_example POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_RUNTIME_DLLS:adbcpp_multi_example>
              $<TARGET_FILE_DIR:adbcpp_multi_example>
      COMMAND_EXPAND_LISTS
    )
    add_custom_command(TARGET adbcpp_demo_example POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_RUNTIME_DLLS:adbcpp_demo_example>
              $<TARGET_FILE_DIR:adbcpp_demo_example>
      COMMAND_EXPAND_LISTS
    )
    add_custom_command(TARGET adbcpp_demo_multi_example POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_RUNTIME_DLLS:adbcpp_demo_multi_example>
              $<TARGET_FILE_DIR:adbcpp_demo_multi_example>
      COMMAND_EXPAND_LISTS
    )
  endif()
endif()