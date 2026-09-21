#if defined(_WIN32)
#    define NOMINMAX
#endif

#include "adbcpp/usb/usb_transport.hpp"

#include <libusb.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "adbcpp/log.hpp"

namespace adbcpp::usb
{
namespace
{

// The ADB function is a vendor-specific USB interface. The class/subclass/
// protocol triple is fixed by Android and is how both adb and adbd locate it
// (see AOSP's `usb_libusb.cpp`). Bulk transfers carry the ADB messages.
constexpr std::uint8_t kAdbInterfaceClass = 0xFF;
constexpr std::uint8_t kAdbInterfaceSubClass = 0x42;
constexpr std::uint8_t kAdbInterfaceProtocol = 0x01;
// The buffer for one bulk transfer. A header and its payload arrive as separate
// transfers, so the buffer holds one of them. It is smaller than the maximum
// payload the peer may send, which is fine: a larger message is read across
// several transfers by `Session`'s read loop, and a smaller buffer keeps every
// transport from holding a full `kMaxData` allocation.
constexpr std::size_t kReadBufferSize = 256 * 1024;

// libusb is a C API, so it reports a failure as a code rather than throwing;
// this turns one into an `Error`.
Error fail(std::string_view what, int code)
{
    return Error{ErrorCode::Transport,
                 std::string(what) + ": " + libusb_strerror(static_cast<enum libusb_error>(code))};
}

// Reads the device's USB `iSerial` descriptor string, which is the serial adb
// prints and selects on. An empty result means the device has no serial descriptor,
// or that it could not be read; the two are not distinguished because the caller
// treats both as "select by model instead".
std::string device_serial(libusb_device *device, const libusb_device_descriptor &descriptor)
{
    if (descriptor.iSerialNumber == 0)
    {
        return {};
    }

    libusb_device_handle *handle = nullptr;
    if (libusb_open(device, &handle) != 0)
    {
        return {};
    }

    std::array<unsigned char, 256> buffer{};
    const int length = libusb_get_string_descriptor_ascii(handle, descriptor.iSerialNumber, buffer.data(),
                                                          static_cast<int>(buffer.size()));
    libusb_close(handle);
    if (length <= 0)
    {
        return {};
    }
    return std::string(reinterpret_cast<const char *>(buffer.data()), static_cast<std::size_t>(length));
}

// Whether `descriptor` can satisfy `id`. A zero vendor or product id matches any,
// so a selector with only a serial finds its device whatever the model.
bool matches(const libusb_device_descriptor &descriptor, DeviceId id)
{
    if (id.vendor_id != 0 && descriptor.idVendor != id.vendor_id)
    {
        return false;
    }
    if (id.product_id != 0 && descriptor.idProduct != id.product_id)
    {
        return false;
    }
    return true;
}

libusb_device *find_device(libusb_device **devices, ssize_t count, DeviceId id)
{
    for (ssize_t i = 0; i < count; ++i)
    {
        libusb_device_descriptor descriptor{};
        if (libusb_get_device_descriptor(devices[i], &descriptor) != 0)
        {
            continue;
        }
        if (!matches(descriptor, id))
        {
            continue;
        }
        // Reading the serial opens the device, so it is only done when a serial was
        // asked for; without one the first model match is taken, as before.
        if (id.serial.empty() || device_serial(devices[i], descriptor) == id.serial)
        {
            return devices[i];
        }
    }
    return nullptr;
}

// The ADB interface and the bulk endpoints it exposes.
struct AdbInterface
{
    int number = -1;
    std::uint8_t endpoint_in = 0;
    std::uint8_t endpoint_out = 0;
};

// Locates the ADB interface (class 0xFF, subclass 0x42, protocol 0x01) and its
// bulk endpoints. An empty optional means the device has no ADB interface, and an
// error means the descriptors could not be read at all.
Result<std::optional<AdbInterface>> find_adb_interface(libusb_device *device)
{
    libusb_config_descriptor *config = nullptr;
    int rc = libusb_get_active_config_descriptor(device, &config);
    if (rc != 0)
    {
        rc = libusb_get_config_descriptor(device, 0, &config);
    }
    if (rc != 0)
    {
        return tl::unexpected(fail("libusb_get_config_descriptor", rc));
    }

    AdbInterface found;
    for (std::uint8_t i = 0; i < config->bNumInterfaces && found.number < 0; ++i)
    {
        const libusb_interface &interface = config->interface[i];
        for (int j = 0; j < interface.num_altsetting; ++j)
        {
            const libusb_interface_descriptor &altsetting = interface.altsetting[j];
            if (altsetting.bInterfaceClass != kAdbInterfaceClass ||
                altsetting.bInterfaceSubClass != kAdbInterfaceSubClass ||
                altsetting.bInterfaceProtocol != kAdbInterfaceProtocol)
            {
                continue;
            }
            found.number = altsetting.bInterfaceNumber;
            for (std::uint8_t k = 0; k < altsetting.bNumEndpoints; ++k)
            {
                const libusb_endpoint_descriptor &endpoint = altsetting.endpoint[k];
                if ((endpoint.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK)
                {
                    continue;
                }
                if ((endpoint.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN)
                {
                    found.endpoint_in = endpoint.bEndpointAddress;
                }
                else
                {
                    found.endpoint_out = endpoint.bEndpointAddress;
                }
            }
            break;
        }
    }
    libusb_free_config_descriptor(config);

    if (found.number < 0 || found.endpoint_in == 0 || found.endpoint_out == 0)
    {
        return std::optional<AdbInterface>{};
    }
    return found;
}

} // namespace

Result<DeviceId> DeviceId::parse(std::string_view text)
{
    constexpr std::string_view kSerialPrefix = "serial:";
    if (text.starts_with(kSerialPrefix))
    {
        if (text.size() == kSerialPrefix.size())
        {
            return tl::unexpected(Error{ErrorCode::InvalidArgument, "serial: needs a serial number"});
        }
        DeviceId id;
        id.serial = std::string(text.substr(kSerialPrefix.size()));
        return id;
    }

    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 == text.size())
    {
        return tl::unexpected(Error{ErrorCode::InvalidArgument, "expected VID:PID or serial:<serial>"});
    }
    try
    {
        DeviceId id;
        id.vendor_id = static_cast<std::uint16_t>(std::stoul(std::string(text.substr(0, colon)), nullptr, 16));
        id.product_id = static_cast<std::uint16_t>(std::stoul(std::string(text.substr(colon + 1)), nullptr, 16));
        return id;
    }
    catch (const std::exception &)
    {
        return tl::unexpected(Error{ErrorCode::InvalidArgument, "VID:PID must be hexadecimal"});
    }
}

Result<bool> UsbTransport::is_present(DeviceId id)
{
    libusb_context *context = nullptr;
    const int initialized = libusb_init(&context);
    if (initialized != 0)
    {
        return tl::unexpected(fail("libusb_init", initialized));
    }

    libusb_device **devices = nullptr;
    const ssize_t count = libusb_get_device_list(context, &devices);
    const bool found = count >= 0 && find_device(devices, count, id) != nullptr;
    if (count >= 0)
    {
        libusb_free_device_list(devices, 1);
    }
    libusb_exit(context);
    return found;
}

Result<std::vector<DeviceId>> UsbTransport::list()
{
    libusb_context *context = nullptr;
    const int initialized = libusb_init(&context);
    if (initialized != 0)
    {
        return tl::unexpected(fail("libusb_init", initialized));
    }

    libusb_device **devices = nullptr;
    const ssize_t count = libusb_get_device_list(context, &devices);
    if (count < 0)
    {
        libusb_exit(context);
        return tl::unexpected(fail("libusb_get_device_list", static_cast<int>(count)));
    }

    std::vector<DeviceId> found;
    for (ssize_t i = 0; i < count; ++i)
    {
        libusb_device_descriptor descriptor{};
        if (libusb_get_device_descriptor(devices[i], &descriptor) != 0)
        {
            continue;
        }
        // A device without an ADB interface is not one of ours, so it is skipped
        // rather than reported and then failing to open.
        const auto adb = find_adb_interface(devices[i]);
        if (!adb || !*adb)
        {
            continue;
        }
        DeviceId id;
        id.vendor_id = descriptor.idVendor;
        id.product_id = descriptor.idProduct;
        id.serial = device_serial(devices[i], descriptor);
        found.push_back(std::move(id));
    }
    libusb_free_device_list(devices, 1);
    libusb_exit(context);
    return found;
}

struct UsbTransport::Impl
{
    libusb_context *context = nullptr;
    libusb_device_handle *handle = nullptr;
    int interface_number = -1;
    std::uint8_t endpoint_in = 0;
    std::uint8_t endpoint_out = 0;
    bool claimed = false;
    std::vector<std::byte> incoming;
    std::size_t incoming_offset = 0;
    // The matched device's `iSerial` descriptor, cached at open so `serial()` does
    // not have to reopen the device.
    std::string serial;
    unsigned int transfer_timeout_ms = kDefaultTransferTimeoutMs;
    unsigned int transfer_budget_ms = kDefaultTransferBudgetMs;

    // Runs one bulk transfer, retrying while it times out having moved nothing.
    //
    // libusb gives up when the peer sends nothing at all for `transfer_timeout_ms`.
    // That silence can be transient, or, during the handshake, the user taking their
    // time to approve the on-device debugging prompt, so the attempt is repeated
    // until the budget runs out. libusb is careful not to lose data it did transfer,
    // and warns not to treat a timeout as proof that nothing moved, so `transferred`
    // decides: nothing moved is retried, and a partial transfer is returned to the
    // caller, which treats a partial read as a short read and a partial write as a
    // desync.
    int bulk_transfer(unsigned char endpoint, unsigned char *data, int length, int *transferred)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(transfer_budget_ms);
        while (true)
        {
            const int rc = libusb_bulk_transfer(handle, endpoint, data, length, transferred, transfer_timeout_ms);
            if (rc != LIBUSB_ERROR_TIMEOUT || *transferred > 0)
            {
                return rc;
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return rc;
            }
        }
    }

    ~Impl()
    {
        if (handle != nullptr)
        {
            if (claimed)
            {
                libusb_release_interface(handle, interface_number);
            }
            libusb_close(handle);
        }
        if (context != nullptr)
        {
            libusb_exit(context);
        }
    }
};

UsbTransport::UsbTransport()
    : impl_(std::make_unique<Impl>())
{
}

UsbTransport::UsbTransport(UsbTransport &&) noexcept = default;
UsbTransport &UsbTransport::operator=(UsbTransport &&) noexcept = default;

Result<UsbTransport> UsbTransport::open(DeviceId id, unsigned int transfer_timeout_ms, unsigned int transfer_budget_ms)
{
    UsbTransport transport;
    transport.impl_->transfer_timeout_ms = transfer_timeout_ms;
    transport.impl_->transfer_budget_ms = transfer_budget_ms;

    int rc = libusb_init(&transport.impl_->context);
    if (rc != 0)
    {
        const auto error = fail("libusb_init", rc);
        log(LogLevel::Error, "could not open the USB device: " + error.message);
        return tl::unexpected(error);
    }

    libusb_device **devices = nullptr;
    const ssize_t count = libusb_get_device_list(transport.impl_->context, &devices);
    if (count < 0)
    {
        const auto error = fail("libusb_get_device_list", static_cast<int>(count));
        log(LogLevel::Error, "could not open the USB device: " + error.message);
        return tl::unexpected(error);
    }

    libusb_device *match = find_device(devices, count, id);

    if (match == nullptr)
    {
        libusb_free_device_list(devices, 1);
        const Error error{ErrorCode::Transport, "no USB device matching the given id"};
        log(LogLevel::Error, "could not open the USB device: " + error.message);
        return tl::unexpected(error);
    }

    const auto adb = find_adb_interface(match);
    if (!adb)
    {
        libusb_free_device_list(devices, 1);
        log(LogLevel::Error, "could not open the USB device: " + adb.error().message);
        return tl::unexpected(adb.error());
    }
    if (!*adb)
    {
        libusb_free_device_list(devices, 1);
        const Error error{ErrorCode::Transport, "ADB USB interface not found"};
        log(LogLevel::Error, "could not open the USB device: " + error.message);
        return tl::unexpected(error);
    }
    transport.impl_->interface_number = (*adb)->number;
    transport.impl_->endpoint_in = (*adb)->endpoint_in;
    transport.impl_->endpoint_out = (*adb)->endpoint_out;

    // The serial is read here, by opening the device once, so `serial()` can report
    // it later without reopening. This is the same descriptor string `adb devices`
    // prints and `DeviceId::serial` matches.
    libusb_device_descriptor descriptor{};
    if (libusb_get_device_descriptor(match, &descriptor) == 0)
    {
        transport.impl_->serial = device_serial(match, descriptor);
    }

    rc = libusb_open(match, &transport.impl_->handle);
    libusb_free_device_list(devices, 1);
    if (rc != 0)
    {
        const auto error = fail("libusb_open", rc);
        log(LogLevel::Error, "could not open the USB device: " + error.message);
        return tl::unexpected(error);
    }

#if defined(__linux__)
    libusb_set_auto_detach_kernel_driver(transport.impl_->handle, 1);
#endif

    // The ADB interface must be claimed before any bulk transfer, and released
    // again on close, so adb and this library do not use it at the same time.
    rc = libusb_claim_interface(transport.impl_->handle, transport.impl_->interface_number);
    if (rc != 0)
    {
        const auto error = fail("libusb_claim_interface", rc);
        log(LogLevel::Error, "could not open the USB device: " + error.message);
        return tl::unexpected(error);
    }
    transport.impl_->claimed = true;

    // A failed transfer can leave a bulk endpoint halted, which makes every later
    // transfer on it fail. Clearing the halt on open recovers from that state
    // (blocker 4 in `04-blockers.md`). The device's own endpoint may also stay
    // halted until the device is replugged, which the host cannot fix.
    libusb_clear_halt(transport.impl_->handle, transport.impl_->endpoint_in);
    libusb_clear_halt(transport.impl_->handle, transport.impl_->endpoint_out);

    log(LogLevel::Info,
        transport.impl_->serial.empty() ? "opened the USB device" : "opened the USB device " + transport.impl_->serial);

    return transport;
}

UsbTransport::~UsbTransport() = default;

void UsbTransport::set_transfer_timeout(unsigned int milliseconds) noexcept
{
    impl_->transfer_timeout_ms = milliseconds;
}

unsigned int UsbTransport::transfer_timeout() const noexcept
{
    return impl_->transfer_timeout_ms;
}

void UsbTransport::set_transfer_budget(unsigned int milliseconds) noexcept
{
    impl_->transfer_budget_ms = milliseconds;
}

unsigned int UsbTransport::transfer_budget() const noexcept
{
    return impl_->transfer_budget_ms;
}

Result<std::size_t> UsbTransport::read(std::span<std::byte> buffer)
{
    // One bulk transfer is read at a time, but the caller may ask for fewer bytes
    // than the transfer carries, so the remainder is buffered and handed out by
    // later reads. `Session` relies on this to read a header and then a payload
    // from two separate transfers.
    if (impl_->incoming_offset >= impl_->incoming.size())
    {
        impl_->incoming.resize(kReadBufferSize);
        int transferred = 0;
        const int rc =
            impl_->bulk_transfer(impl_->endpoint_in, reinterpret_cast<unsigned char *>(impl_->incoming.data()),
                                 static_cast<int>(impl_->incoming.size()), &transferred);
        // A timeout that still delivered bytes is a short read, not a failure: the
        // bytes are used and the caller reads on.
        if (rc != 0 && !(rc == LIBUSB_ERROR_TIMEOUT && transferred > 0))
        {
            return tl::unexpected(fail("libusb_bulk_transfer (read)", rc));
        }
        impl_->incoming.resize(static_cast<std::size_t>(transferred));
        impl_->incoming_offset = 0;
        if (transferred == 0)
        {
            return 0;
        }
    }

    const std::size_t available = impl_->incoming.size() - impl_->incoming_offset;
    const std::size_t count = std::min(available, buffer.size());
    std::copy_n(impl_->incoming.data() + impl_->incoming_offset, count, buffer.data());
    impl_->incoming_offset += count;
    return count;
}

Status UsbTransport::write(std::span<const std::byte> data)
{
    // A zero-length transfer would be a zero-length packet, which some USB stacks
    // reject, so an empty write is simply a no-op.
    if (data.empty())
    {
        return {};
    }

    int transferred = 0;
    const int rc = impl_->bulk_transfer(
        impl_->endpoint_out, const_cast<unsigned char *>(reinterpret_cast<const unsigned char *>(data.data())),
        static_cast<int>(data.size()), &transferred);
    if (rc != 0)
    {
        // A write is never retried once it has moved bytes: the device received a
        // prefix of a message, and sending the rest would desynchronize the stream.
        if (rc == LIBUSB_ERROR_TIMEOUT && transferred > 0)
        {
            return tl::unexpected(Error{ErrorCode::Transport, "short USB write: the stream is desynchronized"});
        }
        return tl::unexpected(fail("libusb_bulk_transfer (write)", rc));
    }
    // A short write means the device received only part of a message, which would
    // desynchronize the stream, so it is treated as an error.
    if (static_cast<std::size_t>(transferred) != data.size())
    {
        return tl::unexpected(Error{ErrorCode::Transport, "short USB write"});
    }
    return {};
}

Result<bool> UsbTransport::wait_readable(std::chrono::milliseconds timeout)
{
    // A previous read may have left bytes buffered, so the endpoint is already
    // readable without another transfer.
    if (impl_->incoming_offset < impl_->incoming.size())
    {
        return true;
    }

    // libusb exposes no pollable handle on Windows, and its poll-fd list is Linux
    // and macOS only, so a bulk transfer bounded by `timeout` is the only portable
    // way to ask the endpoint. It is not retried here: the wait is bounded by
    // `timeout`, not by the transfer budget, and bytes that arrive are buffered
    // for the next `read`, exactly as `read` does.
    impl_->incoming.resize(kReadBufferSize);
    int transferred = 0;
    const int rc = libusb_bulk_transfer(
        impl_->handle, impl_->endpoint_in, reinterpret_cast<unsigned char *>(impl_->incoming.data()),
        static_cast<int>(impl_->incoming.size()), &transferred, static_cast<unsigned int>(timeout.count()));
    // A timeout that still delivered bytes is readable, exactly as in `read`; one
    // that delivered nothing is not.
    if (rc != 0 && !(rc == LIBUSB_ERROR_TIMEOUT && transferred > 0))
    {
        impl_->incoming.clear();
        impl_->incoming_offset = 0;
        return tl::unexpected(fail("libusb_bulk_transfer (wait)", rc));
    }
    impl_->incoming.resize(static_cast<std::size_t>(transferred));
    impl_->incoming_offset = 0;
    // A successful transfer that moved no bytes is a zero-length packet, which
    // `read` reports as the end of the stream, so it is readable too.
    return rc == 0 || transferred > 0;
}

void UsbTransport::close()
{
    if (impl_->handle != nullptr)
    {
        if (impl_->claimed)
        {
            libusb_release_interface(impl_->handle, impl_->interface_number);
            impl_->claimed = false;
        }
        libusb_close(impl_->handle);
        impl_->handle = nullptr;
    }
}

std::string_view UsbTransport::serial() const noexcept
{
    return impl_->serial;
}

} // namespace adbcpp::usb
