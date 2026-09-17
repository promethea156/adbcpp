#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "adbcpp/export.hpp"
#include "adbcpp/transport.hpp"

namespace adbcpp::usb
{

/// Identifies an ADB USB device by vendor and product id.
///
/// Every device exposes the same USB ids for its ADB interface regardless of
/// manufacturer, so a USB vendor/product pair selects one particular model.
struct ADBCPP_API DeviceId
{
    std::uint16_t vendor_id = 0;
    std::uint16_t product_id = 0;
};

/**
 * @brief A Transport over ADB's USB interface, backed by libusb.
 *
 * Opens the device matching the given vendor/product id, locates the ADB
 * interface (class `0xFF`, subclass `0x42`, protocol `0x01`), and uses its
 * bulk endpoints. That interface class/subclass/protocol triple is how both adb
 * and adbd find the ADB function; AOSP matches it in `usb_libusb.cpp`:
 *
 *   https://android.googlesource.com/platform/packages/modules/adb/+/refs/heads/main/client/usb_libusb.cpp
 *
 * Each read returns the contents of one USB transfer, so a header and its payload
 * arrive as separate reads. This is the reason `Session` writes them separately
 * too (blocker 3 in `04-blockers.md`).
 *
 * @note libusb is linked dynamically. This type is only available when
 * `adbcpp` is built with `ADBCPP_BUILD_USB=ON`.
 */
class ADBCPP_API UsbTransport : public Transport
{
public:
    /// The default timeout for each bulk transfer, in milliseconds.
    ///
    /// The timeout applies to **one bulk transfer**, which carries at most one
    /// message header or payload, and not to a whole file transfer. libusb gives up
    /// when the peer sends nothing at all for this long, so the value bounds how long
    /// a stall is tolerated, not how long a chunk takes on the wire. It is deliberately
    /// short, so that a device that has gone quiet is noticed quickly, and a timeout is
    /// retried rather than failing the transfer outright.
    static constexpr unsigned int kDefaultTransferTimeoutMs = 5000;

    /// The default total a single transfer waits before it gives up, in milliseconds.
    ///
    /// A transfer is retried while it times out with nothing transferred, because
    /// silence can be transient, or, during the handshake, the user taking their time
    /// to approve the on-device debugging prompt. This budget bounds that retrying, so
    /// the wait is bounded even though the per-transfer timeout is short.
    static constexpr unsigned int kDefaultTransferBudgetMs = 120000;

    /// Returns whether a USB device matching `id` is currently present.
    static bool is_present(DeviceId id);

    /// @param transfer_timeout_ms The timeout for each bulk transfer, in
    ///        milliseconds. See @ref kDefaultTransferTimeoutMs.
    explicit UsbTransport(DeviceId id, unsigned int transfer_timeout_ms = kDefaultTransferTimeoutMs);
    ~UsbTransport() override;

    UsbTransport(const UsbTransport &) = delete;
    UsbTransport &operator=(const UsbTransport &) = delete;

    /// Sets the timeout applied to each later bulk transfer, in milliseconds.
    void set_transfer_timeout(unsigned int milliseconds) noexcept;

    /// The timeout applied to each bulk transfer, in milliseconds.
    unsigned int transfer_timeout() const noexcept;

    /// Sets the total a single transfer waits before giving up, in milliseconds.
    void set_transfer_budget(unsigned int milliseconds) noexcept;

    /// The total a single transfer waits before giving up, in milliseconds.
    unsigned int transfer_budget() const noexcept;

    std::size_t read(std::span<std::byte> buffer) override;
    void write(std::span<const std::byte> data) override;
    void close() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace adbcpp::usb
