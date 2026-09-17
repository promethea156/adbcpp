add_executable(adbcpp_sample sample/main.cpp)
target_link_libraries(adbcpp_sample PRIVATE adbcpp::adbcpp)

add_executable(adbcpp_sync_example sync/main.cpp)
target_link_libraries(adbcpp_sync_example PRIVATE adbcpp::adbcpp)

# The sample and the sync example are device-free, so CTest runs them as part of
# the normal test suite. The USB example needs a device and is not registered.
if(ADBCPP_BUILD_TESTS)
  add_test(NAME example_sample COMMAND adbcpp_sample)
  add_test(NAME example_sync COMMAND adbcpp_sync_example)
endif()

if(ADBCPP_BUILD_USB)
  add_executable(adbcpp_usb_example usb/main.cpp)
  target_link_libraries(adbcpp_usb_example
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
  endif()
endif()