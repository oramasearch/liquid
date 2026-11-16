// SPDX-License-Identifier: MIT

#include "identity/keys/backend.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <utility>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDManager.h>
#elif defined(__linux__)
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <hidsdi.h>
#include <setupapi.h>
#include <windows.h>
#endif

#include "crypto/crypto.hpp"

namespace liquid::identity_detail {
namespace {

constexpr auto report_size = std::size_t{64};
constexpr auto init_data_size = std::size_t{57};
constexpr auto continuation_data_size = std::size_t{59};
constexpr auto broadcast_channel = std::uint32_t{0xffffffffU};
constexpr auto ctaphid_init = std::uint8_t{0x06};
constexpr auto ctaphid_cbor = std::uint8_t{0x10};
constexpr auto ctaphid_keepalive = std::uint8_t{0x3b};

auto failure(errc code, std::string message, std::string context) -> error {
    return {code, std::move(message), std::move(context)};
}

auto put_channel(std::span<std::byte> output, std::uint32_t channel) -> void {
    output[0] = std::byte{static_cast<std::uint8_t>(channel >> 24U)};
    output[1] = std::byte{static_cast<std::uint8_t>(channel >> 16U)};
    output[2] = std::byte{static_cast<std::uint8_t>(channel >> 8U)};
    output[3] = std::byte{static_cast<std::uint8_t>(channel)};
}

auto get_channel(std::span<const std::byte> input) -> std::uint32_t {
    return (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[0])) << 24U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[1])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[2])) << 8U) |
           static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[3]));
}

class hid_device {
public:
    virtual ~hid_device() = default;
    virtual auto write_report(std::span<const std::byte, report_size> report) -> result<void> = 0;
    virtual auto read_report() -> result<std::array<std::byte, report_size>> = 0;
};

#if defined(__APPLE__)
class apple_hid_device final : public hid_device {
public:
    explicit apple_hid_device(IOHIDDeviceRef device) : device_(device) {}

    ~apple_hid_device() override {
        IOHIDDeviceClose(device_, kIOHIDOptionsTypeNone);
        CFRelease(device_);
    }

    auto write_report(std::span<const std::byte, report_size> report) -> result<void> override {
        const auto status = IOHIDDeviceSetReport(
            device_,
            kIOHIDReportTypeOutput,
            0,
            reinterpret_cast<const std::uint8_t*>(report.data()),
            report.size());
        if (status != kIOReturnSuccess) {
            return std::unexpected(failure(errc::io, "FIDO HID write failed", "IOHID"));
        }
        return {};
    }

    auto read_report() -> result<std::array<std::byte, report_size>> override {
        auto output = std::array<std::byte, report_size>{};
        auto size = static_cast<CFIndex>(output.size());
        const auto status = IOHIDDeviceGetReport(
            device_,
            kIOHIDReportTypeInput,
            0,
            reinterpret_cast<std::uint8_t*>(output.data()),
            &size);
        if (status != kIOReturnSuccess || size != static_cast<CFIndex>(output.size())) {
            return std::unexpected(failure(errc::io, "FIDO HID read failed", "IOHID"));
        }
        return output;
    }

private:
    IOHIDDeviceRef device_;
};

auto open_hid_device() -> result<std::unique_ptr<hid_device>> {
    auto manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    auto match = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    auto page_value = std::int32_t{0xf1d0};
    auto usage_value = std::int32_t{1};
    auto page = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &page_value);
    auto usage = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &usage_value);
    CFDictionarySetValue(match, CFSTR(kIOHIDDeviceUsagePageKey), page);
    CFDictionarySetValue(match, CFSTR(kIOHIDDeviceUsageKey), usage);
    IOHIDManagerSetDeviceMatching(manager, match);
    const auto status = IOHIDManagerOpen(manager, kIOHIDOptionsTypeNone);
    auto devices = IOHIDManagerCopyDevices(manager);
    CFRelease(usage);
    CFRelease(page);
    CFRelease(match);
    IOHIDManagerClose(manager, kIOHIDOptionsTypeNone);
    CFRelease(manager);
    if (status != kIOReturnSuccess || !devices || CFSetGetCount(devices) == 0) {
        if (devices) {
            CFRelease(devices);
        }
        return std::unexpected(
            failure(errc::unsupported, "no FIDO2 HID authenticator is available", "IOHID"));
    }
    auto values = std::vector<const void*>(static_cast<std::size_t>(CFSetGetCount(devices)));
    CFSetGetValues(devices, values.data());
    auto device = static_cast<IOHIDDeviceRef>(const_cast<void*>(values.front()));
    CFRetain(device);
    CFRelease(devices);
    if (IOHIDDeviceOpen(device, kIOHIDOptionsTypeNone) != kIOReturnSuccess) {
        CFRelease(device);
        return std::unexpected(
            failure(errc::unsupported, "cannot open FIDO2 HID authenticator", "IOHID"));
    }
    return std::make_unique<apple_hid_device>(device);
}
#elif defined(__linux__)
class linux_hid_device final : public hid_device {
public:
    explicit linux_hid_device(int descriptor) : descriptor_(descriptor) {}

    ~linux_hid_device() override {
        ::close(descriptor_);
    }

    auto write_report(std::span<const std::byte, report_size> report) -> result<void> override {
        auto output = std::array<std::byte, report_size + 1>{};
        std::ranges::copy(report, output.begin() + 1);
        const auto count = ::write(descriptor_, output.data(), output.size());
        if (count != static_cast<ssize_t>(output.size())) {
            return std::unexpected(
                failure(errc::io, std::strerror(errno), "FIDO2 hidraw write"));
        }
        return {};
    }

    auto read_report() -> result<std::array<std::byte, report_size>> override {
        auto event = pollfd{descriptor_, POLLIN, 0};
        if (::poll(&event, 1, 30000) <= 0) {
            return std::unexpected(failure(errc::io, "FIDO2 HID read timed out", "hidraw"));
        }
        auto output = std::array<std::byte, report_size>{};
        const auto count = ::read(descriptor_, output.data(), output.size());
        if (count != static_cast<ssize_t>(output.size())) {
            return std::unexpected(
                failure(errc::io, std::strerror(errno), "FIDO2 hidraw read"));
        }
        return output;
    }

private:
    int descriptor_;
};

auto open_hid_device() -> result<std::unique_ptr<hid_device>> {
    constexpr auto usage = std::array<unsigned char, 5>{0x06, 0xd0, 0xf1, 0x09, 0x01};
    for (auto index = 0; index < 64; ++index) {
        const auto path = "/dev/hidraw" + std::to_string(index);
        const auto descriptor = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (descriptor < 0) {
            continue;
        }
        auto report_size = int{};
        auto report = hidraw_report_descriptor{};
        if (::ioctl(descriptor, HIDIOCGRDESCSIZE, &report_size) >= 0 && report_size > 0 &&
            report_size <= HID_MAX_DESCRIPTOR_SIZE) {
            report.size = static_cast<__u32>(report_size);
            if (::ioctl(descriptor, HIDIOCGRDESC, &report) >= 0 &&
                std::search(report.value,
                            report.value + report.size,
                            usage.begin(),
                            usage.end()) != report.value + report.size) {
                return std::make_unique<linux_hid_device>(descriptor);
            }
        }
        ::close(descriptor);
    }
    return std::unexpected(
        failure(errc::unsupported, "no FIDO2 HID authenticator is available", "hidraw"));
}
#elif defined(_WIN32)
class windows_hid_device final : public hid_device {
public:
    windows_hid_device(HANDLE device, USHORT input_size, USHORT output_size)
        : device_(device), input_size_(input_size), output_size_(output_size) {}

    ~windows_hid_device() override {
        CloseHandle(device_);
    }

    auto write_report(std::span<const std::byte, report_size> report) -> result<void> override {
        auto output = std::vector<std::byte>(output_size_);
        if (output.size() < report.size() + 1) {
            return std::unexpected(
                failure(errc::unsupported, "FIDO HID output report is too small", "Windows HID"));
        }
        std::ranges::copy(report, output.begin() + 1);
        auto written = DWORD{};
        if (!WriteFile(device_, output.data(), static_cast<DWORD>(output.size()), &written, nullptr) ||
            written != output.size()) {
            return std::unexpected(failure(errc::io, "FIDO HID write failed", "Windows HID"));
        }
        return {};
    }

    auto read_report() -> result<std::array<std::byte, report_size>> override {
        auto input = std::vector<std::byte>(input_size_);
        auto read = DWORD{};
        if (!ReadFile(device_, input.data(), static_cast<DWORD>(input.size()), &read, nullptr) ||
            read < report_size + 1) {
            return std::unexpected(failure(errc::io, "FIDO HID read failed", "Windows HID"));
        }
        auto output = std::array<std::byte, report_size>{};
        std::ranges::copy_n(input.begin() + 1, output.size(), output.begin());
        return output;
    }

private:
    HANDLE device_;
    USHORT input_size_;
    USHORT output_size_;
};

auto open_hid_device() -> result<std::unique_ptr<hid_device>> {
    auto guid = GUID{};
    HidD_GetHidGuid(&guid);
    const auto devices = SetupDiGetClassDevs(
        &guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) {
        return std::unexpected(
            failure(errc::unsupported, "Windows HID enumeration failed", "Windows HID"));
    }
    for (auto index = DWORD{};; ++index) {
        auto interface_data = SP_DEVICE_INTERFACE_DATA{};
        interface_data.cbSize = sizeof(interface_data);
        if (!SetupDiEnumDeviceInterfaces(devices, nullptr, &guid, index, &interface_data)) {
            break;
        }
        auto required = DWORD{};
        SetupDiGetDeviceInterfaceDetail(
            devices, &interface_data, nullptr, 0, &required, nullptr);
        auto storage = std::vector<std::byte>(required);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA*>(storage.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
        if (!SetupDiGetDeviceInterfaceDetail(
                devices, &interface_data, detail, required, nullptr, nullptr)) {
            continue;
        }
        const auto device = CreateFile(detail->DevicePath,
                                       GENERIC_READ | GENERIC_WRITE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       nullptr,
                                       OPEN_EXISTING,
                                       0,
                                       nullptr);
        if (device == INVALID_HANDLE_VALUE) {
            continue;
        }
        auto preparsed = PHIDP_PREPARSED_DATA{};
        auto capabilities = HIDP_CAPS{};
        const auto matched = HidD_GetPreparsedData(device, &preparsed) &&
                             HidP_GetCaps(preparsed, &capabilities) == HIDP_STATUS_SUCCESS &&
                             capabilities.UsagePage == 0xf1d0 && capabilities.Usage == 1;
        if (preparsed) {
            HidD_FreePreparsedData(preparsed);
        }
        if (matched) {
            SetupDiDestroyDeviceInfoList(devices);
            return std::make_unique<windows_hid_device>(device,
                                                        capabilities.InputReportByteLength,
                                                        capabilities.OutputReportByteLength);
        }
        CloseHandle(device);
    }
    SetupDiDestroyDeviceInfoList(devices);
    return std::unexpected(
        failure(errc::unsupported, "no FIDO2 HID authenticator is available", "Windows HID"));
}
#else
auto open_hid_device() -> result<std::unique_ptr<hid_device>> {
    return std::unexpected(failure(errc::unsupported,
                                   "no FIDO2 HID authenticator is available",
                                   "operating-system HID"));
}
#endif

auto write_message(hid_device& device,
                   std::uint32_t channel,
                   std::uint8_t command,
                   std::span<const std::byte> payload) -> result<void> {
    auto report = std::array<std::byte, report_size>{};
    put_channel(report, channel);
    report[4] = std::byte{static_cast<std::uint8_t>(command | 0x80U)};
    report[5] = std::byte{static_cast<std::uint8_t>(payload.size() >> 8U)};
    report[6] = std::byte{static_cast<std::uint8_t>(payload.size())};
    auto copied = std::min(payload.size(), init_data_size);
    std::ranges::copy(payload.first(copied), report.begin() + 7);
    auto written = device.write_report(report);
    if (!written) {
        return written;
    }
    auto offset = copied;
    auto sequence = std::uint8_t{};
    while (offset < payload.size()) {
        report.fill(std::byte{});
        put_channel(report, channel);
        report[4] = std::byte{sequence++};
        copied = std::min(payload.size() - offset, continuation_data_size);
        std::ranges::copy(payload.subspan(offset, copied), report.begin() + 5);
        written = device.write_report(report);
        if (!written) {
            return written;
        }
        offset += copied;
    }
    return {};
}

auto read_message(hid_device& device, std::uint32_t channel, std::uint8_t command)
    -> result<std::vector<std::byte>> {
    for (;;) {
        auto first = device.read_report();
        if (!first) {
            return std::unexpected(first.error());
        }
        if (get_channel(*first) != channel ||
            std::to_integer<std::uint8_t>((*first)[4]) != (command | 0x80U)) {
            if (std::to_integer<std::uint8_t>((*first)[4]) == (ctaphid_keepalive | 0x80U)) {
                continue;
            }
            return std::unexpected(failure(errc::corrupt, "unexpected CTAPHID response", "HID"));
        }
        const auto size =
            (static_cast<std::size_t>(std::to_integer<std::uint8_t>((*first)[5])) << 8U) |
            std::to_integer<std::uint8_t>((*first)[6]);
        auto output = std::vector<std::byte>{};
        output.reserve(size);
        const auto first_size = std::min(size, init_data_size);
        output.insert(output.end(), first->begin() + 7, first->begin() + 7 + first_size);
        auto sequence = std::uint8_t{};
        while (output.size() < size) {
            auto next = device.read_report();
            if (!next || get_channel(*next) != channel ||
                std::to_integer<std::uint8_t>((*next)[4]) != sequence++) {
                return std::unexpected(next ?
                                           failure(errc::corrupt,
                                                   "invalid CTAPHID continuation",
                                                   "HID") :
                                           next.error());
            }
            const auto copied = std::min(size - output.size(), continuation_data_size);
            output.insert(output.end(), next->begin() + 5, next->begin() + 5 + copied);
        }
        return output;
    }
}

auto initialize(hid_device& device) -> result<std::uint32_t> {
    auto nonce = std::array<std::byte, 8>{};
    auto random = crypto_detail::random_bytes(nonce);
    if (!random) {
        return std::unexpected(random.error());
    }
    auto written = write_message(device, broadcast_channel, ctaphid_init, nonce);
    if (!written) {
        return std::unexpected(written.error());
    }
    auto response = read_message(device, broadcast_channel, ctaphid_init);
    if (!response || response->size() < 17 ||
        !std::ranges::equal(nonce, std::span{*response}.first(8))) {
        return std::unexpected(response ? failure(errc::corrupt, "invalid CTAPHID init", "HID") :
                                          response.error());
    }
    return get_channel(std::span{*response}.subspan(8, 4));
}

class operating_system_transport final : public fido2_transport {
public:
    auto transact(std::uint8_t command, std::span<const std::byte> payload)
        -> result<std::vector<std::byte>> override {
        auto device = open_hid_device();
        if (!device) {
            return std::unexpected(device.error());
        }
        auto channel = initialize(**device);
        if (!channel) {
            return std::unexpected(channel.error());
        }
        auto request = std::vector<std::byte>{std::byte{command}};
        request.insert(request.end(), payload.begin(), payload.end());
        auto written = write_message(**device, *channel, ctaphid_cbor, request);
        if (!written) {
            return std::unexpected(written.error());
        }
        auto response = read_message(**device, *channel, ctaphid_cbor);
        if (!response || response->empty()) {
            return std::unexpected(response ? failure(errc::corrupt, "empty CTAP2 response", "HID") :
                                              response.error());
        }
        if (response->front() != std::byte{}) {
            return std::unexpected(failure(errc::unauthorized,
                                           "CTAP2 authenticator refused the operation",
                                           std::to_string(std::to_integer<unsigned>(response->front()))));
        }
        return std::vector<std::byte>{response->begin() + 1, response->end()};
    }
};

} // namespace

auto operating_system_fido2_transport() -> std::shared_ptr<fido2_transport> {
    return std::make_shared<operating_system_transport>();
}

} // namespace liquid::identity_detail
