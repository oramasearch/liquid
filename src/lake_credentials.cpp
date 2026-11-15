// The operating system's credential store for lake tokens. macOS: a generic password in the
// login keychain (service "liquid lake", account the lake's key). Windows: a generic credential
// named "liquid-lake:<key>". Elsewhere there is no store liquid writes to; tokens come from the
// environment (docs/library/lakes.md).

#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

#include "liquid/lake.hpp"

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#elif defined(_WIN32)
#define NOMINMAX
#include <wincred.h>
#include <windows.h>
#endif

namespace liquid {
namespace {

auto store_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

class unavailable_store final : public lake_credential_store {
public:
    explicit unavailable_store(std::string reason) : reason_(std::move(reason)) {}

    auto read(std::string_view) -> result<std::optional<std::string>> override {
        return std::optional<std::string>{};
    }

    auto write(std::string_view, std::string_view) -> result<void> override {
        return std::unexpected(store_error(errc::unavailable, reason_, "credential store"));
    }

    auto erase(std::string_view) -> result<void> override {
        return {};
    }

private:
    std::string reason_;
};

#ifdef __APPLE__
constexpr auto service_name = "liquid lake";

class cf_ref {
public:
    explicit cf_ref(CFTypeRef value = nullptr) : value_(value) {}
    ~cf_ref() {
        if (value_) {
            CFRelease(value_);
        }
    }
    cf_ref(const cf_ref&) = delete;
    cf_ref& operator=(const cf_ref&) = delete;

    [[nodiscard]] auto get() const -> CFTypeRef {
        return value_;
    }

private:
    CFTypeRef value_;
};

auto cf_string(std::string_view value) -> CFStringRef {
    return CFStringCreateWithBytes(nullptr,
                                   reinterpret_cast<const UInt8*>(value.data()),
                                   static_cast<CFIndex>(value.size()),
                                   kCFStringEncodingUTF8,
                                   false);
}

auto base_query(CFStringRef service, CFStringRef account) -> CFMutableDictionaryRef {
    auto* query = CFDictionaryCreateMutable(
        nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, service);
    CFDictionarySetValue(query, kSecAttrAccount, account);
    return query;
}

auto status_error(OSStatus status, std::string_view action) -> error {
    return store_error(errc::io,
                       "the keychain refused to " + std::string{action} + " the lake token",
                       "OSStatus " + std::to_string(status));
}

class keychain_store final : public lake_credential_store {
public:
    auto read(std::string_view origin) -> result<std::optional<std::string>> override {
        auto service = cf_ref{cf_string(service_name)};
        auto account = cf_ref{cf_string(origin)};
        auto query = cf_ref{base_query(static_cast<CFStringRef>(service.get()),
                                       static_cast<CFStringRef>(account.get()))};
        auto* mutable_query =
            const_cast<CFMutableDictionaryRef>(static_cast<CFDictionaryRef>(query.get()));
        CFDictionarySetValue(mutable_query, kSecReturnData, kCFBooleanTrue);
        CFDictionarySetValue(mutable_query, kSecMatchLimit, kSecMatchLimitOne);
        CFTypeRef found = nullptr;
        const auto status = SecItemCopyMatching(static_cast<CFDictionaryRef>(query.get()), &found);
        auto owned = cf_ref{found};
        if (status == errSecItemNotFound) {
            return std::optional<std::string>{};
        }
        if (status != errSecSuccess || !found || CFGetTypeID(found) != CFDataGetTypeID()) {
            return std::unexpected(status_error(status, "read"));
        }
        const auto* data = static_cast<CFDataRef>(found);
        return std::optional<std::string>{
            std::string{reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
                        static_cast<std::size_t>(CFDataGetLength(data))}};
    }

    auto write(std::string_view origin, std::string_view token) -> result<void> override {
        auto service = cf_ref{cf_string(service_name)};
        auto account = cf_ref{cf_string(origin)};
        auto data = cf_ref{CFDataCreate(nullptr,
                                        reinterpret_cast<const UInt8*>(token.data()),
                                        static_cast<CFIndex>(token.size()))};
        auto query = cf_ref{base_query(static_cast<CFStringRef>(service.get()),
                                       static_cast<CFStringRef>(account.get()))};
        auto* update = CFDictionaryCreateMutable(
            nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        auto owned_update = cf_ref{update};
        CFDictionarySetValue(update, kSecValueData, data.get());
        auto status = SecItemUpdate(static_cast<CFDictionaryRef>(query.get()), update);
        if (status == errSecItemNotFound) {
            auto* mutable_query =
                const_cast<CFMutableDictionaryRef>(static_cast<CFDictionaryRef>(query.get()));
            CFDictionarySetValue(mutable_query, kSecValueData, data.get());
            status = SecItemAdd(static_cast<CFDictionaryRef>(query.get()), nullptr);
        }
        if (status != errSecSuccess) {
            return std::unexpected(status_error(status, "store"));
        }
        return {};
    }

    auto erase(std::string_view origin) -> result<void> override {
        auto service = cf_ref{cf_string(service_name)};
        auto account = cf_ref{cf_string(origin)};
        auto query = cf_ref{base_query(static_cast<CFStringRef>(service.get()),
                                       static_cast<CFStringRef>(account.get()))};
        const auto status = SecItemDelete(static_cast<CFDictionaryRef>(query.get()));
        if (status != errSecSuccess && status != errSecItemNotFound) {
            return std::unexpected(status_error(status, "remove"));
        }
        return {};
    }
};
#elif defined(_WIN32)
auto target_name(std::string_view origin) -> std::wstring {
    const auto text = "liquid-lake:" + std::string{origin};
    const auto size =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    auto output = std::wstring(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), output.data(), size);
    return output;
}

class credential_manager_store final : public lake_credential_store {
public:
    auto read(std::string_view origin) -> result<std::optional<std::string>> override {
        auto target = target_name(origin);
        PCREDENTIALW credential = nullptr;
        if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
            if (GetLastError() == ERROR_NOT_FOUND) {
                return std::optional<std::string>{};
            }
            return std::unexpected(store_error(
                errc::io, "Credential Manager refused the read", std::to_string(GetLastError())));
        }
        auto output = std::string(reinterpret_cast<const char*>(credential->CredentialBlob),
                                  credential->CredentialBlobSize);
        CredFree(credential);
        return std::optional<std::string>{std::move(output)};
    }

    auto write(std::string_view origin, std::string_view token) -> result<void> override {
        auto target = target_name(origin);
        auto blob = std::string{token};
        auto credential = CREDENTIALW{};
        credential.Type = CRED_TYPE_GENERIC;
        credential.TargetName = target.data();
        credential.CredentialBlobSize = static_cast<DWORD>(blob.size());
        credential.CredentialBlob = reinterpret_cast<LPBYTE>(blob.data());
        credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
        if (!CredWriteW(&credential, 0)) {
            return std::unexpected(store_error(
                errc::io, "Credential Manager refused the write", std::to_string(GetLastError())));
        }
        return {};
    }

    auto erase(std::string_view origin) -> result<void> override {
        auto target = target_name(origin);
        if (!CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0) &&
            GetLastError() != ERROR_NOT_FOUND) {
            return std::unexpected(store_error(errc::io,
                                               "Credential Manager refused the removal",
                                               std::to_string(GetLastError())));
        }
        return {};
    }
};
#endif

} // namespace

auto os_lake_credential_store() -> std::shared_ptr<lake_credential_store> {
    if (const auto* configured = std::getenv("LIQUID_CREDENTIAL_STORE");
        configured && std::string_view{configured} == "none") {
        return std::make_shared<unavailable_store>(
            "the credential store is off (LIQUID_CREDENTIAL_STORE=none)");
    }
#ifdef __APPLE__
    return std::make_shared<keychain_store>();
#elif defined(_WIN32)
    return std::make_shared<credential_manager_store>();
#else
    return std::make_shared<unavailable_store>(
        "liquid has no operating-system credential store on this platform");
#endif
}

} // namespace liquid
