#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "adbcpp/export.hpp"
#include "adbcpp/transport.hpp"

namespace adbcpp::usb {

/// Identifies an ADB USB device by vendor and product id.
struct ADBCPP_API DeviceId {
  std::uint16_t vendor_id = 0;
  std::uint16_t product_id = 0;
};

/**
 * @brief A Transport over ADB's USB interface, backed by libusb.
 *
 * Opens the device matching the given vendor/product id, locates the ADB
 * interface (class `0xFF`, subclass `0x42`, protocol `0x01`), and uses its
 * bulk endpoints. Each read returns the contents of one USB transfer, so a
 * header and its payload arrive as separate reads.
 *
 * @note libusb is linked dynamically. This type is only available when
 * `adbcpp` is built with `ADBCPP_BUILD_USB=ON`.
 */
class ADBCPP_API UsbTransport : public Transport {
public:
  /// Returns whether a USB device matching `id` is currently present.
  static bool is_present(DeviceId id);

  explicit UsbTransport(DeviceId id);
  ~UsbTransport() override;

  UsbTransport(const UsbTransport &) = delete;
  UsbTransport &operator=(const UsbTransport &) = delete;

  std::size_t read(std::span<std::byte> buffer) override;
  void write(std::span<const std::byte> data) override;
  void close() override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace adbcpp::usb
