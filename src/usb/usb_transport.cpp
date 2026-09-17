#if defined(_WIN32)
#define NOMINMAX
#endif

#include "adbcpp/usb/usb_transport.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

#include <libusb.h>

namespace adbcpp::usb {
namespace {

constexpr std::uint8_t kAdbInterfaceClass = 0xFF;
constexpr std::uint8_t kAdbInterfaceSubClass = 0x42;
constexpr std::uint8_t kAdbInterfaceProtocol = 0x01;
// Long enough to wait for the user to approve the on-device debugging prompt.
constexpr unsigned int kTransferTimeoutMs = 120000;
constexpr std::size_t kReadBufferSize = 256 * 1024;

[[noreturn]] void fail(const std::string &what, int code) {
  throw std::runtime_error("adbcpp: " + what + ": " +
                          libusb_strerror(static_cast<enum libusb_error>(code)));
}

libusb_device *find_device(libusb_device **devices, ssize_t count,
                           DeviceId id) {
  for (ssize_t i = 0; i < count; ++i) {
    libusb_device_descriptor descriptor{};
    if (libusb_get_device_descriptor(devices[i], &descriptor) != 0) {
      continue;
    }
    if (descriptor.idVendor == id.vendor_id &&
        descriptor.idProduct == id.product_id) {
      return devices[i];
    }
  }
  return nullptr;
}

} // namespace

bool UsbTransport::is_present(DeviceId id) {
  libusb_context *context = nullptr;
  if (libusb_init(&context) != 0) {
    return false;
  }

  libusb_device **devices = nullptr;
  const ssize_t count = libusb_get_device_list(context, &devices);
  const bool found =
      count >= 0 && find_device(devices, count, id) != nullptr;
  if (count >= 0) {
    libusb_free_device_list(devices, 1);
  }
  libusb_exit(context);
  return found;
}

struct UsbTransport::Impl {
  libusb_context *context = nullptr;
  libusb_device_handle *handle = nullptr;
  int interface_number = -1;
  std::uint8_t endpoint_in = 0;
  std::uint8_t endpoint_out = 0;
  bool claimed = false;
  std::vector<std::byte> incoming;
  std::size_t incoming_offset = 0;

  ~Impl() {
    if (handle != nullptr) {
      if (claimed) {
        libusb_release_interface(handle, interface_number);
      }
      libusb_close(handle);
    }
    if (context != nullptr) {
      libusb_exit(context);
    }
  }
};

UsbTransport::UsbTransport(DeviceId id) : impl_(std::make_unique<Impl>()) {
  int rc = libusb_init(&impl_->context);
  if (rc != 0) {
    fail("libusb_init", rc);
  }

  libusb_device **devices = nullptr;
  const ssize_t count = libusb_get_device_list(impl_->context, &devices);
  if (count < 0) {
    fail("libusb_get_device_list", static_cast<int>(count));
  }

  libusb_device *match = find_device(devices, count, id);

  if (match == nullptr) {
    libusb_free_device_list(devices, 1);
    throw std::runtime_error("adbcpp: no USB device matching the given id");
  }

  libusb_config_descriptor *config = nullptr;
  rc = libusb_get_active_config_descriptor(match, &config);
  if (rc != 0) {
    rc = libusb_get_config_descriptor(match, 0, &config);
  }
  if (rc != 0) {
    libusb_free_device_list(devices, 1);
    fail("libusb_get_config_descriptor", rc);
  }

  for (std::uint8_t i = 0;
       i < config->bNumInterfaces && impl_->interface_number < 0; ++i) {
    const libusb_interface &interface = config->interface[i];
    for (int j = 0; j < interface.num_altsetting; ++j) {
      const libusb_interface_descriptor &altsetting = interface.altsetting[j];
      if (altsetting.bInterfaceClass != kAdbInterfaceClass ||
          altsetting.bInterfaceSubClass != kAdbInterfaceSubClass ||
          altsetting.bInterfaceProtocol != kAdbInterfaceProtocol) {
        continue;
      }
      impl_->interface_number = altsetting.bInterfaceNumber;
      for (std::uint8_t k = 0; k < altsetting.bNumEndpoints; ++k) {
        const libusb_endpoint_descriptor &endpoint = altsetting.endpoint[k];
        if ((endpoint.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) !=
            LIBUSB_TRANSFER_TYPE_BULK) {
          continue;
        }
        if ((endpoint.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) ==
            LIBUSB_ENDPOINT_IN) {
          impl_->endpoint_in = endpoint.bEndpointAddress;
        } else {
          impl_->endpoint_out = endpoint.bEndpointAddress;
        }
      }
      break;
    }
  }

  libusb_free_config_descriptor(config);

  if (impl_->interface_number < 0 || impl_->endpoint_in == 0 ||
      impl_->endpoint_out == 0) {
    libusb_free_device_list(devices, 1);
    throw std::runtime_error("adbcpp: ADB USB interface not found");
  }

  rc = libusb_open(match, &impl_->handle);
  libusb_free_device_list(devices, 1);
  if (rc != 0) {
    fail("libusb_open", rc);
  }

#if defined(__linux__)
  libusb_set_auto_detach_kernel_driver(impl_->handle, 1);
#endif

  rc = libusb_claim_interface(impl_->handle, impl_->interface_number);
  if (rc != 0) {
    fail("libusb_claim_interface", rc);
  }
  impl_->claimed = true;

  libusb_clear_halt(impl_->handle, impl_->endpoint_in);
  libusb_clear_halt(impl_->handle, impl_->endpoint_out);
}

UsbTransport::~UsbTransport() = default;

std::size_t UsbTransport::read(std::span<std::byte> buffer) {
  if (impl_->incoming_offset >= impl_->incoming.size()) {
    impl_->incoming.resize(kReadBufferSize);
    int transferred = 0;
    const int rc = libusb_bulk_transfer(
        impl_->handle, impl_->endpoint_in,
        reinterpret_cast<unsigned char *>(impl_->incoming.data()),
        static_cast<int>(impl_->incoming.size()), &transferred,
        kTransferTimeoutMs);
    if (rc != 0) {
      fail("libusb_bulk_transfer", rc);
    }
    impl_->incoming.resize(static_cast<std::size_t>(transferred));
    impl_->incoming_offset = 0;
    if (transferred == 0) {
      return 0;
    }
  }

  const std::size_t available =
      impl_->incoming.size() - impl_->incoming_offset;
  const std::size_t count = std::min(available, buffer.size());
  std::copy_n(impl_->incoming.begin() +
                  static_cast<std::ptrdiff_t>(impl_->incoming_offset),
              static_cast<std::ptrdiff_t>(count), buffer.begin());
  impl_->incoming_offset += count;
  return count;
}

void UsbTransport::write(std::span<const std::byte> data) {
  if (data.empty()) {
    return;
  }

  int transferred = 0;
  const int rc = libusb_bulk_transfer(
      impl_->handle, impl_->endpoint_out,
      const_cast<unsigned char *>(
          reinterpret_cast<const unsigned char *>(data.data())),
      static_cast<int>(data.size()), &transferred, kTransferTimeoutMs);
  if (rc != 0) {
    fail("libusb_bulk_transfer", rc);
  }
  if (static_cast<std::size_t>(transferred) != data.size()) {
    throw std::runtime_error("adbcpp: short USB write");
  }
}

void UsbTransport::close() {
  if (impl_->handle != nullptr) {
    if (impl_->claimed) {
      libusb_release_interface(impl_->handle, impl_->interface_number);
      impl_->claimed = false;
    }
    libusb_close(impl_->handle);
    impl_->handle = nullptr;
  }
}

} // namespace adbcpp::usb
