// SPDX-License-Identifier: MIT

#include <cstring>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif

#include "crypto/crypto.hpp"
#include "identity/custody/common.hpp"
#include "identity/custody/providers.hpp"

namespace liquid::custody_detail {
namespace {

#if defined(__APPLE__)

// Every item liquid writes is a generic password under this service; the account is the custody
// URI's parameter, so one Keychain holds independent keys for independent liquid homes.
constexpr std::string_view keychain_service = "dev.liquid.custody";

struct cf_release {
    void operator()(const void* value) const noexcept {
        if (value != nullptr) {
            CFRelease(value);
        }
    }
};

template <class Reference>
using cf_owned = std::unique_ptr<std::remove_pointer_t<Reference>, cf_release>;

auto cf_string(std::string_view value) -> cf_owned<CFStringRef> {
    return cf_owned<CFStringRef>{
        CFStringCreateWithBytes(kCFAllocatorDefault,
                                reinterpret_cast<const UInt8*>(value.data()),
                                static_cast<CFIndex>(value.size()),
                                kCFStringEncodingUTF8,
                                false)};
}

auto keychain_failure(OSStatus status, std::string_view operation, const std::string& account)
    -> error {
    return custody_error(errc::unauthorized,
                         std::format("macOS Keychain {} failed (OSStatus {})", operation, status),
                         "keychain:" + account);
}

auto item_query(CFStringRef service, CFStringRef account) -> cf_owned<CFMutableDictionaryRef> {
    auto query = cf_owned<CFMutableDictionaryRef>{CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks)};
    if (!query) {
        return query;
    }
    CFDictionarySetValue(query.get(), kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query.get(), kSecAttrService, service);
    CFDictionarySetValue(query.get(), kSecAttrAccount, account);
    return query;
}

// Reads the key-encryption key stored for `account`. `errc::not_found` means no item exists.
auto read_item(const std::string& account) -> result<crypto_detail::key> {
    auto service = cf_string(keychain_service);
    auto name = cf_string(account);
    if (!service || !name) {
        return std::unexpected(custody_error(
            errc::internal, "cannot encode Keychain item name", "keychain:" + account));
    }
    auto query = item_query(service.get(), name.get());
    if (!query) {
        return std::unexpected(
            custody_error(errc::internal, "cannot build Keychain query", "keychain:" + account));
    }
    CFDictionarySetValue(query.get(), kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query.get(), kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef found = nullptr;
    const auto status = SecItemCopyMatching(query.get(), &found);
    auto owned = cf_owned<CFTypeRef>{found};
    if (status == errSecItemNotFound) {
        return std::unexpected(custody_error(
            errc::not_found, "no Keychain item for this custody", "keychain:" + account));
    }
    if (status != errSecSuccess) {
        return std::unexpected(keychain_failure(status, "read", account));
    }
    if (found == nullptr || CFGetTypeID(found) != CFDataGetTypeID()) {
        return std::unexpected(
            custody_error(errc::corrupt, "Keychain item is not data", "keychain:" + account));
    }
    const auto* data = static_cast<CFDataRef>(found);
    auto output = crypto_detail::key{};
    if (CFDataGetLength(data) != static_cast<CFIndex>(output.size())) {
        return std::unexpected(custody_error(
            errc::corrupt, "Keychain item does not hold a 32-byte key", "keychain:" + account));
    }
    std::memcpy(output.data(), CFDataGetBytePtr(data), output.size());
    return output;
}

// Adds a new item. It is not marked synchronizable, so it does not leave this Mac through iCloud.
auto add_item(const std::string& account, const crypto_detail::key& value) -> result<void> {
    auto service = cf_string(keychain_service);
    auto name = cf_string(account);
    auto label = cf_string("liquid key-encryption key");
    if (!service || !name || !label) {
        return std::unexpected(custody_error(
            errc::internal, "cannot encode Keychain item name", "keychain:" + account));
    }
    auto data = cf_owned<CFDataRef>{CFDataCreate(kCFAllocatorDefault,
                                                 reinterpret_cast<const UInt8*>(value.data()),
                                                 static_cast<CFIndex>(value.size()))};
    auto query = item_query(service.get(), name.get());
    if (!data || !query) {
        return std::unexpected(
            custody_error(errc::internal, "cannot build Keychain item", "keychain:" + account));
    }
    CFDictionarySetValue(query.get(), kSecAttrLabel, label.get());
    CFDictionarySetValue(query.get(), kSecValueData, data.get());
    const auto status = SecItemAdd(query.get(), nullptr);
    if (status != errSecSuccess) {
        return std::unexpected(keychain_failure(status, "add", account));
    }
    return {};
}

auto read_or_create_item(const std::string& account) -> result<crypto_detail::key> {
    auto existing = read_item(account);
    if (existing || existing.error().code != errc::not_found) {
        return existing;
    }
    auto created = crypto_detail::random_key();
    if (!created) {
        return std::unexpected(created.error());
    }
    auto added = add_item(account, *created);
    if (!added) {
        crypto_detail::wipe(*created);
        // Another process may have added the item first; use whichever key the Keychain holds.
        auto raced = read_item(account);
        if (raced) {
            return raced;
        }
        return std::unexpected(added.error());
    }
    return created;
}

// Holds a random key-encryption key in the macOS login Keychain and seals data keys under it with
// XChaCha20-Poly1305, bound to the wrap context. The wrapped bytes never contain the KEK.
class keychain_provider final : public key_provider {
public:
    explicit keychain_provider(std::string account) : account_(std::move(account)) {}

    auto wrap(std::span<const std::byte, 32> data_key, wrap_context context)
        -> runtime::task<std::vector<std::byte>> override {
        auto kek = co_await runtime::offload(
            [account = account_]() { return read_or_create_item(account); });
        if (!kek) {
            co_return std::unexpected(kek.error());
        }
        if (!*kek) {
            co_return std::unexpected(kek->error());
        }
        auto sealed = crypto_detail::seal(**kek, as_bytes(reference(context)), data_key);
        crypto_detail::wipe(**kek);
        co_return sealed;
    }

    auto unwrap(std::span<const std::byte> wrapped, wrap_context context)
        -> runtime::task<key> override {
        auto kek = co_await runtime::offload([account = account_]() { return read_item(account); });
        if (!kek) {
            co_return std::unexpected(kek.error());
        }
        if (!*kek) {
            co_return std::unexpected(kek->error());
        }
        auto opened = crypto_detail::open(**kek, as_bytes(reference(context)), wrapped);
        crypto_detail::wipe(**kek);
        if (!opened || opened->size() != key{}.size()) {
            if (opened) {
                crypto_detail::wipe(*opened);
            }
            co_return std::unexpected(custody_error(
                errc::unauthorized, "cannot unwrap Keychain data key", reference(context)));
        }
        auto output = key{};
        std::memcpy(output.data(), opened->data(), output.size());
        crypto_detail::wipe(*opened);
        co_return output;
    }

    auto custody_kind() const noexcept -> std::string_view override {
        return "keychain";
    }

protected:
    auto parameters() const -> std::vector<std::byte> override {
        return bytes(account_);
    }

private:
    std::string account_;
};

#endif

} // namespace

auto make_keychain_provider(std::string account) -> result<std::shared_ptr<key_provider>> {
    if (account.empty()) {
        return std::unexpected(custody_error(
            errc::invalid_argument, "keychain custody URI must name an account", "keychain:"));
    }
#if !defined(__APPLE__)
    return std::unexpected(custody_error(
        errc::unsupported, "keychain custody is only available on macOS", "keychain:" + account));
#else
    return std::make_shared<keychain_provider>(std::move(account));
#endif
}

} // namespace liquid::custody_detail
