#include <catch2/catch_test_macros.hpp>
#include <string>

#include "adbcpp/usb/usb_transport.hpp"

// `DeviceId::parse` is the only part of the USB backend that needs no device, so
// it is tested without one: the selectors are the two forms adb also accepts.
TEST_CASE("DeviceId::parse reads a VID:PID selector", "[usb]")
{
    const auto id = adbcpp::usb::DeviceId::parse("22D9:2769");
    REQUIRE(id.has_value());
    REQUIRE(id->vendor_id == 0x22D9);
    REQUIRE(id->product_id == 0x2769);
    REQUIRE(id->serial.empty());
}

TEST_CASE("DeviceId::parse reads a serial selector", "[usb]")
{
    const auto id = adbcpp::usb::DeviceId::parse("serial:3B15AD001NS00000");
    REQUIRE(id.has_value());
    REQUIRE(id->vendor_id == 0);
    REQUIRE(id->product_id == 0);
    REQUIRE(id->serial == "3B15AD001NS00000");
}

TEST_CASE("DeviceId::parse rejects a selector without a colon", "[usb]")
{
    const auto id = adbcpp::usb::DeviceId::parse("22D9");
    REQUIRE_FALSE(id.has_value());
    REQUIRE(id.error().code == adbcpp::ErrorCode::InvalidArgument);
}

TEST_CASE("DeviceId::parse rejects an empty serial selector", "[usb]")
{
    const auto id = adbcpp::usb::DeviceId::parse("serial:");
    REQUIRE_FALSE(id.has_value());
    REQUIRE(id.error().code == adbcpp::ErrorCode::InvalidArgument);
}

TEST_CASE("DeviceId::parse rejects a non-hexadecimal VID:PID", "[usb]")
{
    const auto id = adbcpp::usb::DeviceId::parse("nope:2769");
    REQUIRE_FALSE(id.has_value());
    REQUIRE(id.error().code == adbcpp::ErrorCode::InvalidArgument);
}
