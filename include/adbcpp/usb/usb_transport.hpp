#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "adbcpp/error.hpp"
#include "adbcpp/export.hpp"
#include "adbcpp/transport.hpp"

namespace adbcpp::usb
{

/// Identifies an ADB USB device, by model and optionally by USB serial number.
///
/// Every device exposes the same USB ids for its ADB interface regardless of
/// manufacturer, so a vendor/product pair selects one particular model. Two devices
/// of that model are then told apart by `serial`, which is the USB `iSerial`
/// descriptor string, the same value adb prints in `adb devices`.
///
/// A default-constructed id matches the first ADB device, and one with only
/// `serial` set matches by serial alone, so a caller does not need to know the ids
/// of a device whose serial they have.
struct ADBCPP_API DeviceId
{
    /// The USB vendor id, or zero to match any vendor.
    std::uint16_t vendor_id = 0;
    /// The USB product id, or zero to match any product.
    std::uint16_t product_id = 0;
    /// The USB serial number, or empty to match the first matching device.
    ///
    /// A device with no `iSerial` descriptor has an empty serial, so it can only be
    /// selected as part of a model, or through its banner `serialno`
    /// (`Connection::device_serial`) when the model is ambiguous.
    std::string serial;

    /**
     * @brief Parses a selector: `VID:PID` (hex) or `serial:<serial>`.
     *
     * `VID:PID` selects a model and leaves @ref serial empty; `serial:<serial>`
     * selects a serial and leaves the ids zero, so it matches the device whatever
     * its model. Anything else is an `InvalidArgument`.
     */
    static Result<DeviceId> parse(std::string_view text);
};

/**
 * @brief A Transport over ADB's USB interface, backed by libusb.
 *
 * Opens the device matching the given @ref DeviceId, by its serial when one is
 * set, locates the ADB interface (class `0xFF`, subclass `0x42`, protocol
 * `0x01`), and uses its bulk endpoints. That interface class/subclass/protocol
 * triple is how both adb and adbd find the ADB function; AOSP matches it in
 * `usb_libusb.cpp`:
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

    /**
     * @brief Returns whether a USB device matching `id` is currently present.
     *
     * An error is returned when libusb itself cannot be initialized, which used
     * to be indistinguishable from "no device is attached".
     */
    static Result<bool> is_present(DeviceId id);

    /**
     * @brief Returns every attached ADB device, each with its USB serial filled in.
     *
     * This is how a caller discovers the serial of a device to pass to @ref open,
     * since a USB serial cannot be known in advance. The list is in libusb's
     * enumeration order, which is stable for one attached set. A device whose USB
     * descriptor has no serial is still listed, with an empty @ref DeviceId::serial.
     *
     * @note A device that is claimed by a running adb server is still listed,
     * because enumeration does not claim it, but @ref open then fails.
     */
    static Result<std::vector<DeviceId>> list();

    /**
     * @brief Opens the USB device matching `id`.
     *
     * Opening can fail, and a constructor cannot report that, so this is a named
     * factory and the constructor is private. Both timings are taken here rather
     * than set afterwards, so the transport is fully configured before it exists.
     *
     * When @ref DeviceId::serial is empty the first device matching the ids is
     * opened; otherwise the device's USB serial must match it too, which is how two
     * identical devices are told apart. A device with no serial descriptor cannot
     * be selected by serial here, and its banner `serialno` is the fallback.
     *
     * @param transfer_timeout_ms The timeout for each bulk transfer, in
     *        milliseconds. See @ref kDefaultTransferTimeoutMs.
     * @param transfer_budget_ms The total a single transfer waits before giving
     *        up, in milliseconds. See @ref kDefaultTransferBudgetMs.
     */
    static Result<UsbTransport> open(DeviceId id, unsigned int transfer_timeout_ms = kDefaultTransferTimeoutMs,
                                     unsigned int transfer_budget_ms = kDefaultTransferBudgetMs);

    ~UsbTransport() override;

    UsbTransport(const UsbTransport &) = delete;
    UsbTransport &operator=(const UsbTransport &) = delete;

    /// A transport is returned by value, so it moves.
    UsbTransport(UsbTransport &&) noexcept;
    UsbTransport &operator=(UsbTransport &&) noexcept;

    /// Sets the timeout applied to each later bulk transfer, in milliseconds.
    void set_transfer_timeout(unsigned int milliseconds) noexcept;

    /// The timeout applied to each bulk transfer, in milliseconds.
    unsigned int transfer_timeout() const noexcept;

    /// Sets the total a single transfer waits before giving up, in milliseconds.
    void set_transfer_budget(unsigned int milliseconds) noexcept;

    /// The total a single transfer waits before giving up, in milliseconds.
    unsigned int transfer_budget() const noexcept;

    Result<std::size_t> read(std::span<std::byte> buffer) override;
    Status write(std::span<const std::byte> data) override;
    void close() override;

    /// Waits until the endpoint has bytes to read, up to `timeout`. Returns
    /// whether it is readable, so the caller can wait on several transports at
    /// once (see @ref adbcpp::wait_readable).
    ///
    /// libusb exposes no pollable handle on Windows, and its poll-fd list is
    /// Linux and macOS only, so the wait is a bulk transfer bounded by `timeout`.
    /// Bytes that arrive are buffered for the next @ref read, exactly as @ref read
    /// does. A transfer that times out having moved nothing is not readable.
    Result<bool> wait_readable(std::chrono::milliseconds timeout) override;

    /// The opened device's USB `iSerial` descriptor, or empty when it has none.
    std::string_view serial() const noexcept override;

private:
    // Opening is done by `open`, so the constructor is private.
    UsbTransport();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace adbcpp::usb
