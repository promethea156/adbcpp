#if defined(_WIN32)
#    define NOMINMAX
#endif

#include "adbcpp/usb/usb_transport.hpp"

#include <libusb.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
// One bulk transfer holds at most one message header or payload. This must be at
// least as large as the maximum payload the peer may send.
constexpr std::size_t kReadBufferSize = 256 * 1024;

// libusb is a C API, so it reports a failure as a code rather than throwing;
// this turns one into an `Error`.
Error fail(std::string_view what, int code)
{
    return Error{ErrorCode::Transport,
                 std::string(what) + ": " + libusb_strerror(static_cast<enum libusb_error>(code))};
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
        if (descriptor.idVendor == id.vendor_id && descriptor.idProduct == id.product_id)
        {
            return devices[i];
        }
    }
    return nullptr;
}

} // namespace

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
        return tl::unexpected(fail("libusb_init", rc));
    }

    libusb_device **devices = nullptr;
    const ssize_t count = libusb_get_device_list(transport.impl_->context, &devices);
    if (count < 0)
    {
        return tl::unexpected(fail("libusb_get_device_list", static_cast<int>(count)));
    }

    libusb_device *match = find_device(devices, count, id);

    if (match == nullptr)
    {
        libusb_free_device_list(devices, 1);
        return tl::unexpected(Error{ErrorCode::Transport, "no USB device matching the given id"});
    }

    libusb_config_descriptor *config = nullptr;
    rc = libusb_get_active_config_descriptor(match, &config);
    if (rc != 0)
    {
        rc = libusb_get_config_descriptor(match, 0, &config);
    }
    if (rc != 0)
    {
        libusb_free_device_list(devices, 1);
        return tl::unexpected(fail("libusb_get_config_descriptor", rc));
    }

    for (std::uint8_t i = 0; i < config->bNumInterfaces && transport.impl_->interface_number < 0; ++i)
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
            transport.impl_->interface_number = altsetting.bInterfaceNumber;
            for (std::uint8_t k = 0; k < altsetting.bNumEndpoints; ++k)
            {
                const libusb_endpoint_descriptor &endpoint = altsetting.endpoint[k];
                if ((endpoint.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK)
                {
                    continue;
                }
                if ((endpoint.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN)
                {
                    transport.impl_->endpoint_in = endpoint.bEndpointAddress;
                }
                else
                {
                    transport.impl_->endpoint_out = endpoint.bEndpointAddress;
                }
            }
            break;
        }
    }

    libusb_free_config_descriptor(config);

    if (transport.impl_->interface_number < 0 || transport.impl_->endpoint_in == 0 ||
        transport.impl_->endpoint_out == 0)
    {
        libusb_free_device_list(devices, 1);
        return tl::unexpected(Error{ErrorCode::Transport, "ADB USB interface not found"});
    }

    rc = libusb_open(match, &transport.impl_->handle);
    libusb_free_device_list(devices, 1);
    if (rc != 0)
    {
        return tl::unexpected(fail("libusb_open", rc));
    }

#if defined(__linux__)
    libusb_set_auto_detach_kernel_driver(transport.impl_->handle, 1);
#endif

    // The ADB interface must be claimed before any bulk transfer, and released
    // again on close, so adb and this library do not use it at the same time.
    rc = libusb_claim_interface(transport.impl_->handle, transport.impl_->interface_number);
    if (rc != 0)
    {
        return tl::unexpected(fail("libusb_claim_interface", rc));
    }
    transport.impl_->claimed = true;

    // A failed transfer can leave a bulk endpoint halted, which makes every later
    // transfer on it fail. Clearing the halt on open recovers from that state
    // (blocker 4 in `04-blockers.md`). The device's own endpoint may also stay
    // halted until the device is replugged, which the host cannot fix.
    libusb_clear_halt(transport.impl_->handle, transport.impl_->endpoint_in);
    libusb_clear_halt(transport.impl_->handle, transport.impl_->endpoint_out);

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
            return tl::unexpected(fail("libusb_bulk_transfer", rc));
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
    std::copy_n(impl_->incoming.begin() + static_cast<std::ptrdiff_t>(impl_->incoming_offset),
                static_cast<std::ptrdiff_t>(count), buffer.begin());
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
        return tl::unexpected(fail("libusb_bulk_transfer", rc));
    }
    // A short write means the device received only part of a message, which would
    // desynchronize the stream, so it is treated as an error.
    if (static_cast<std::size_t>(transferred) != data.size())
    {
        return tl::unexpected(Error{ErrorCode::Transport, "short USB write"});
    }
    return {};
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

} // namespace adbcpp::usb
