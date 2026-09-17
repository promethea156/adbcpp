include(FetchContent)

set(LIBUSB_INSTALL_TARGETS OFF)
set(LIBUSB_BUILD_SHARED_LIBS ON)
# libusb's udev backend includes <libudev.h>, which is not part of a default Linux
# toolchain, so it is turned off: the netlink backend enumerates and watches devices
# without any system package, which keeps the build dependency-free (blocker 20).
set(LIBUSB_ENABLE_UDEV OFF)
FetchContent_Declare(
  libusb_cmake
  GIT_REPOSITORY https://github.com/libusb/libusb-cmake.git
  GIT_TAG v1.0.30-0
)
FetchContent_MakeAvailable(libusb_cmake)

add_library(adbcpp-usb usb_transport.cpp)
add_library(adbcpp::usb ALIAS adbcpp-usb)

# Export the target as `adbcpp::usb`, matching the in-tree alias.
set_target_properties(adbcpp-usb PROPERTIES EXPORT_NAME usb)

target_compile_features(adbcpp-usb PUBLIC cxx_std_20)
target_compile_definitions(adbcpp-usb PRIVATE ADBCPP_EXPORTS)
target_link_libraries(adbcpp-usb
  PUBLIC adbcpp::adbcpp
  PRIVATE usb-1.0
)

if(MSVC)
  target_compile_options(adbcpp-usb PRIVATE /W4 /permissive-)
else()
  target_compile_options(adbcpp-usb PRIVATE -Wall -Wextra -Wpedantic)
endif()
