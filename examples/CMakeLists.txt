add_executable(adbcpp_sample sample/main.cpp)
target_link_libraries(adbcpp_sample PRIVATE adbcpp::adbcpp)

if(ADBCPP_BUILD_USB)
  add_executable(adbcpp_usb_example usb/main.cpp)
  target_link_libraries(adbcpp_usb_example PRIVATE adbcpp::usb)

  add_custom_command(TARGET adbcpp_usb_example POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:adbcpp_usb_example>
            $<TARGET_FILE_DIR:adbcpp_usb_example>
    COMMAND_EXPAND_LISTS
  )
endif()
