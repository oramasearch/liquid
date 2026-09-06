#include "identity/custody/tpm.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <utility>

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <tbs.h>
#include <windows.h>
#endif

#include "crypto/crypto.hpp"
#include "identity/custody/common.hpp"

namespace liquid::custody_detail {
namespace {

constexpr std::uint32_t tpm2_create = 0x00000153;
constexpr std::uint32_t tpm2_create_primary = 0x00000131;
constexpr std::uint32_t tpm2_flush_context = 0x00000165;
constexpr std::uint32_t tpm2_load = 0x00000157;
constexpr std::uint32_t tpm2_policy_get_digest = 0x00000189;
constexpr std::uint32_t tpm2_unseal = 0x0000015e;
constexpr std::uint32_t tpm2_policy_pcr = 0x0000017f;
constexpr std::uint32_t tpm2_start_auth_session = 0x00000176;
constexpr std::uint16_t tpm_st_no_sessions = 0x8001;
constexpr std::uint16_t tpm_st_sessions = 0x8002;
constexpr std::uint32_t tpm_rh_owner = 0x40000001;
constexpr std::uint32_t tpm_rh_null = 0x40000007;
constexpr std::uint32_t tpm_rs_pw = 0x40000009;

auto failure(errc code, std::string message, std::string context) -> error {
    return {code, std::move(message), std::move(context)};
}

auto put_u32(std::vector<std::byte>& output, std::uint32_t value) -> void {
    output.push_back(std::byte{static_cast<std::uint8_t>(value >> 24)});
    output.push_back(std::byte{static_cast<std::uint8_t>(value >> 16)});
    output.push_back(std::byte{static_cast<std::uint8_t>(value >> 8)});
    output.push_back(std::byte{static_cast<std::uint8_t>(value)});
}

auto put_u16(std::vector<std::byte>& output, std::uint16_t value) -> void {
    output.push_back(std::byte{static_cast<std::uint8_t>(value >> 8)});
    output.push_back(std::byte{static_cast<std::uint8_t>(value)});
}

auto get_u32(std::span<const std::byte> input, std::size_t offset) -> std::uint32_t {
    if (offset + 4 > input.size()) {
        return 0;
    }
    return (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[offset])) << 24) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[offset + 1])) << 16) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[offset + 2])) << 8) |
           static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[offset + 3]));
}

auto command(std::uint16_t tag, std::uint32_t code, std::span<const std::byte> body)
    -> std::vector<std::byte> {
    auto output = std::vector<std::byte>{};
    put_u16(output, tag);
    put_u32(output, static_cast<std::uint32_t>(10 + body.size()));
    put_u32(output, code);
    output.insert(output.end(), body.begin(), body.end());
    return output;
}

auto sized(std::span<const std::byte> value) -> std::vector<std::byte> {
    auto output = std::vector<std::byte>{};
    put_u16(output, static_cast<std::uint16_t>(value.size()));
    output.insert(output.end(), value.begin(), value.end());
    return output;
}

auto password_session() -> std::vector<std::byte> {
    auto output = std::vector<std::byte>{};
    put_u32(output, tpm_rs_pw);
    put_u16(output, 0);
    output.push_back(std::byte{});
    put_u16(output, 0);
    return output;
}

auto authorized_command(std::uint32_t code,
                        std::span<const std::uint32_t> handles,
                        std::span<const std::byte> parameters,
                        std::span<const std::byte> authorization) -> std::vector<std::byte> {
    auto body = std::vector<std::byte>{};
    for (const auto handle : handles) {
        put_u32(body, handle);
    }
    put_u32(body, static_cast<std::uint32_t>(authorization.size()));
    body.insert(body.end(), authorization.begin(), authorization.end());
    body.insert(body.end(), parameters.begin(), parameters.end());
    return command(tpm_st_sessions, code, body);
}

auto read_sized(std::span<const std::byte> input, std::size_t& offset)
    -> result<std::span<const std::byte>> {
    if (offset + 2 > input.size()) {
        return std::unexpected(failure(errc::corrupt, "truncated TPM2B value", "TPM 2.0"));
    }
    const auto size =
        (static_cast<std::size_t>(std::to_integer<std::uint8_t>(input[offset])) << 8U) |
        std::to_integer<std::uint8_t>(input[offset + 1]);
    offset += 2;
    if (offset + size > input.size()) {
        return std::unexpected(failure(errc::corrupt, "invalid TPM2B length", "TPM 2.0"));
    }
    const auto output = input.subspan(offset, size);
    offset += size;
    return output;
}

auto response_body(std::span<const std::byte> response, std::string_view operation)
    -> result<std::span<const std::byte>> {
    if (response.size() < 10 || get_u32(response, 2) != response.size()) {
        return std::unexpected(
            failure(errc::corrupt, "invalid TPM 2.0 response", std::string{operation}));
    }
    if (const auto status = get_u32(response, 6); status != 0) {
        return std::unexpected(failure(errc::unauthorized,
                                       "TPM 2.0 policy or authorization failed",
                                       std::string{operation} + ":0x" +
                                           std::to_string(status)));
    }
    return response.subspan(10);
}

class unsupported_transport final : public tpm_transport {
public:
    auto submit(std::span<const std::byte>) -> result<std::vector<std::byte>> override {
#if defined(__APPLE__)
        return std::unexpected(failure(errc::unsupported,
                                       "TPM custody is unavailable on macOS; Secure Enclave is a different device",
                                       "tpm:pcrs"));
#else
        return std::unexpected(
            failure(errc::unsupported, "TPM 2.0 transport is unavailable", "tpm:pcrs"));
#endif
    }
};

#if defined(__linux__)
class linux_transport final : public tpm_transport {
public:
    ~linux_transport() override {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
    }

    auto submit(std::span<const std::byte> request) -> result<std::vector<std::byte>> override {
        auto lock = std::scoped_lock{mutex_};
        if (descriptor_ < 0) {
            descriptor_ = ::open("/dev/tpmrm0", O_RDWR | O_CLOEXEC);
        }
        if (descriptor_ < 0) {
            return std::unexpected(failure(
                errno == ENOENT ? errc::unsupported : errc::io,
                std::strerror(errno),
                "/dev/tpmrm0"));
        }
        auto offset = std::size_t{};
        while (offset < request.size()) {
            const auto count =
                ::write(descriptor_, request.data() + offset, request.size() - offset);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                const auto message = std::string{std::strerror(errno)};
                return std::unexpected(failure(errc::io, message, "/dev/tpmrm0"));
            }
            offset += static_cast<std::size_t>(count);
        }
        auto output = std::vector<std::byte>(4096);
        const auto count = ::read(descriptor_, output.data(), output.size());
        const auto read_error = errno;
        if (count <= 0) {
            return std::unexpected(
                failure(errc::io, std::strerror(read_error), "/dev/tpmrm0"));
        }
        output.resize(static_cast<std::size_t>(count));
        return output;
    }

private:
    std::mutex mutex_;
    int descriptor_{-1};
};
#elif defined(_WIN32)
class windows_transport final : public tpm_transport {
public:
    ~windows_transport() override {
        if (context_ != 0) {
            Tbsip_Context_Close(context_);
        }
    }

    auto submit(std::span<const std::byte> request) -> result<std::vector<std::byte>> override {
        auto lock = std::scoped_lock{mutex_};
        if (context_ == 0) {
            auto parameters = TBS_CONTEXT_PARAMS2{};
            parameters.version = TBS_CONTEXT_VERSION_TWO;
            parameters.includeTpm20 = 1;
            if (Tbsi_Context_Create(
                    reinterpret_cast<PCTBS_CONTEXT_PARAMS>(&parameters), &context_) != TBS_SUCCESS) {
                return std::unexpected(
                    failure(errc::unsupported, "Windows TBS has no TPM 2.0 device", "TBS"));
            }
        }
        auto output = std::vector<std::byte>(4096);
        auto size = static_cast<UINT32>(output.size());
        const auto status = Tbsip_Submit_Command(
            context_,
            TBS_COMMAND_LOCALITY_ZERO,
            TBS_COMMAND_PRIORITY_NORMAL,
            reinterpret_cast<PCBYTE>(request.data()),
            static_cast<UINT32>(request.size()),
            reinterpret_cast<PBYTE>(output.data()),
            &size);
        if (status != TBS_SUCCESS) {
            return std::unexpected(failure(errc::io, "Windows TBS command failed", "TBS"));
        }
        output.resize(size);
        return output;
    }

private:
    std::mutex mutex_;
    TBS_HCONTEXT context_{};
};
#endif

auto transport_mutex() -> std::mutex& {
    static auto value = std::mutex{};
    return value;
}

auto installed_transport() -> std::shared_ptr<tpm_transport>& {
    static auto value = std::shared_ptr<tpm_transport>{};
    return value;
}

auto selected_transport() -> std::shared_ptr<tpm_transport> {
    auto lock = std::scoped_lock{transport_mutex()};
    if (installed_transport()) {
        return installed_transport();
    }
#if defined(__linux__)
    return std::make_shared<linux_transport>();
#elif defined(_WIN32)
    return std::make_shared<windows_transport>();
#else
    return std::make_shared<unsupported_transport>();
#endif
}

auto parse_pcrs(std::string_view parameters) -> result<std::vector<std::uint8_t>> {
    constexpr std::string_view prefix = "pcrs=";
    if (!parameters.starts_with(prefix)) {
        return std::unexpected(
            failure(errc::invalid_argument, "TPM custody requires pcrs=<list>", std::string{parameters}));
    }
    auto output = std::vector<std::uint8_t>{};
    auto remaining = parameters.substr(prefix.size());
    while (!remaining.empty()) {
        const auto separator = remaining.find(',');
        const auto item = remaining.substr(0, separator);
        if (item.empty() || item.size() > 2 ||
            !std::ranges::all_of(item, [](char value) { return value >= '0' && value <= '9'; })) {
            return std::unexpected(
                failure(errc::invalid_argument, "invalid TPM PCR list", std::string{parameters}));
        }
        const auto value = std::stoi(std::string{item});
        if (value > 23 || std::ranges::contains(output, static_cast<std::uint8_t>(value))) {
            return std::unexpected(
                failure(errc::invalid_argument, "invalid TPM PCR list", std::string{parameters}));
        }
        output.push_back(static_cast<std::uint8_t>(value));
        if (separator == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(separator + 1);
    }
    std::ranges::sort(output);
    if (output.empty()) {
        return std::unexpected(
            failure(errc::invalid_argument, "TPM PCR list must not be empty", std::string{parameters}));
    }
    return output;
}

auto pcr_selection(std::span<const std::uint8_t> pcrs) -> std::vector<std::byte> {
    auto bitmap = std::array<std::byte, 3>{};
    for (const auto pcr : pcrs) {
        bitmap[pcr / 8] |= std::byte{static_cast<std::uint8_t>(1U << (pcr % 8))};
    }
    auto output = std::vector<std::byte>{};
    put_u16(output, 0);
    put_u32(output, 1);
    put_u16(output, 0x000b);
    output.push_back(std::byte{3});
    output.insert(output.end(), bitmap.begin(), bitmap.end());
    return output;
}

auto storage_public() -> std::vector<std::byte> {
    auto area = std::vector<std::byte>{};
    put_u16(area, 0x0001);
    put_u16(area, 0x000b);
    put_u32(area, 0x00030472);
    put_u16(area, 0);
    put_u16(area, 0x0006);
    put_u16(area, 128);
    put_u16(area, 0x0043);
    put_u16(area, 0x0010);
    put_u16(area, 2048);
    put_u32(area, 0);
    put_u16(area, 0);
    return sized(area);
}

auto sealed_public(std::span<const std::byte> policy) -> std::vector<std::byte> {
    auto area = std::vector<std::byte>{};
    put_u16(area, 0x0008);
    put_u16(area, 0x000b);
    put_u32(area, 0x00000492);
    auto encoded_policy = sized(policy);
    area.insert(area.end(), encoded_policy.begin(), encoded_policy.end());
    put_u16(area, 0x0010);
    put_u16(area, 0);
    return sized(area);
}

auto policy_authorization(std::uint32_t session) -> std::vector<std::byte> {
    auto output = std::vector<std::byte>{};
    put_u32(output, session);
    put_u16(output, 0);
    output.push_back(std::byte{1});
    put_u16(output, 0);
    return output;
}

class tpm_provider final : public key_provider {
public:
    tpm_provider(std::string parameters,
                 std::vector<std::uint8_t> pcrs,
                 std::shared_ptr<tpm_transport> transport)
        : parameters_(std::move(parameters)),
          pcrs_(std::move(pcrs)),
          transport_(std::move(transport)) {}

    auto wrap(std::span<const std::byte, 32> data_key, wrap_context context)
        -> runtime::task<std::vector<std::byte>> override {
        auto primary = co_await create_primary();
        if (!primary) {
            co_return std::unexpected(primary.error());
        }
        auto session = co_await start_policy_session();
        if (!session) {
            co_await flush(*primary);
            co_return std::unexpected(session.error());
        }
        auto applied = co_await apply_pcr_policy(*session);
        auto policy = applied ? co_await policy_digest(*session) : result<std::vector<std::byte>>{
                                                                    std::unexpected(applied.error())};
        if (!policy || policy->size() != 32) {
            co_await flush(*session);
            co_await flush(*primary);
            co_return std::unexpected(policy ? failure(errc::corrupt,
                                                       "invalid TPM PCR policy digest",
                                                       reference(context))
                                                : policy.error());
        }
        auto sensitive = std::vector<std::byte>{};
        put_u16(sensitive, 0);
        auto encoded_key = sized(data_key);
        sensitive.insert(sensitive.end(), encoded_key.begin(), encoded_key.end());
        auto parameters = sized(sensitive);
        auto public_area = sealed_public(*policy);
        parameters.insert(parameters.end(), public_area.begin(), public_area.end());
        put_u16(parameters, 0);
        put_u32(parameters, 0);
        const auto handles = std::array{*primary};
        const auto authorization = password_session();
        auto created = co_await submit(
            authorized_command(tpm2_create, handles, parameters, authorization), "TPM2_Create");
        co_await flush(*session);
        co_await flush(*primary);
        if (!created || created->size() < 4) {
            co_return std::unexpected(created ?
                                          failure(errc::corrupt,
                                                  "invalid TPM sealed object",
                                                  reference(context)) :
                                          created.error());
        }
        auto offset = std::size_t{4};
        auto private_blob = read_sized(*created, offset);
        auto public_blob = private_blob ? read_sized(*created, offset) :
                                         result<std::span<const std::byte>>{
                                             std::unexpected(private_blob.error())};
        if (!private_blob || !public_blob) {
            co_return std::unexpected(private_blob ? public_blob.error() : private_blob.error());
        }
        auto output = bytes("LQTPM1");
        auto stored_private = sized(*private_blob);
        auto stored_public = sized(*public_blob);
        output.insert(output.end(), stored_private.begin(), stored_private.end());
        output.insert(output.end(), stored_public.begin(), stored_public.end());
        co_return output;
    }

    auto unwrap(std::span<const std::byte> wrapped, wrap_context context)
        -> runtime::task<key> override {
        const auto magic = as_bytes("LQTPM1");
        if (wrapped.size() < magic.size() || !std::ranges::equal(wrapped.first(magic.size()), magic)) {
            co_return std::unexpected(
                failure(errc::corrupt, "invalid TPM sealed-object record", reference(context)));
        }
        auto offset = magic.size();
        auto private_blob = read_sized(wrapped, offset);
        auto public_blob = private_blob ? read_sized(wrapped, offset) :
                                         result<std::span<const std::byte>>{
                                             std::unexpected(private_blob.error())};
        if (!private_blob || !public_blob || offset != wrapped.size()) {
            co_return std::unexpected(private_blob && public_blob ?
                                          failure(errc::corrupt,
                                                  "invalid TPM sealed-object record",
                                                  reference(context)) :
                                          (private_blob ? public_blob.error() : private_blob.error()));
        }
        auto primary = co_await create_primary();
        if (!primary) {
            co_return std::unexpected(primary.error());
        }
        auto parameters = sized(*private_blob);
        auto encoded_public = sized(*public_blob);
        parameters.insert(parameters.end(), encoded_public.begin(), encoded_public.end());
        const auto primary_handles = std::array{*primary};
        const auto password = password_session();
        auto loaded = co_await submit(
            authorized_command(tpm2_load, primary_handles, parameters, password), "TPM2_Load");
        if (!loaded || loaded->size() < 4) {
            co_await flush(*primary);
            co_return std::unexpected(loaded ?
                                          failure(errc::corrupt,
                                                  "invalid TPM loaded-object handle",
                                                  reference(context)) :
                                          loaded.error());
        }
        const auto object = get_u32(*loaded, 0);
        auto session = co_await start_policy_session();
        if (!session) {
            co_await flush(object);
            co_await flush(*primary);
            co_return std::unexpected(session.error());
        }
        auto applied = co_await apply_pcr_policy(*session);
        if (!applied) {
            co_await flush(*session);
            co_await flush(object);
            co_await flush(*primary);
            co_return std::unexpected(applied.error());
        }
        const auto object_handles = std::array{object};
        const auto authorization = policy_authorization(*session);
        auto unsealed = co_await submit(
            authorized_command(tpm2_unseal, object_handles, {}, authorization), "TPM2_Unseal");
        co_await flush(*session);
        co_await flush(object);
        co_await flush(*primary);
        auto result_offset = std::size_t{4};
        auto value = unsealed ? read_sized(*unsealed, result_offset) :
                                result<std::span<const std::byte>>{
                                    std::unexpected(unsealed.error())};
        if (!value || value->size() != 32) {
            co_return std::unexpected(unsealed ? failure(errc::corrupt,
                                                         "invalid TPM unsealed key",
                                                         reference(context))
                                                  : unsealed.error());
        }
        auto output = key{};
        std::ranges::copy(*value, output.begin());
        co_return output;
    }

    auto custody_kind() const noexcept -> std::string_view override {
        return "tpm";
    }

protected:
    auto parameters() const -> std::vector<std::byte> override {
        const auto value = as_bytes(parameters_);
        return {value.begin(), value.end()};
    }

private:
    auto submit(std::vector<std::byte> request, std::string operation)
        -> runtime::task<std::vector<std::byte>> {
        auto response = co_await runtime::offload(
            [transport = transport_, request = std::move(request)]() mutable {
                return transport->submit(request);
            });
        if (!response) {
            co_return std::unexpected(response.error());
        }
        if (!*response) {
            co_return std::unexpected(response->error());
        }
        auto parsed = response_body(**response, operation);
        if (!parsed) {
            co_return std::unexpected(parsed.error());
        }
        co_return std::vector<std::byte>{parsed->begin(), parsed->end()};
    }

    auto create_primary() -> runtime::task<std::uint32_t> {
        auto sensitive = std::vector<std::byte>{};
        put_u16(sensitive, 0);
        put_u16(sensitive, 0);
        auto parameters = sized(sensitive);
        auto public_area = storage_public();
        parameters.insert(parameters.end(), public_area.begin(), public_area.end());
        put_u16(parameters, 0);
        put_u32(parameters, 0);
        const auto handles = std::array{tpm_rh_owner};
        const auto authorization = password_session();
        auto response = co_await submit(
            authorized_command(tpm2_create_primary, handles, parameters, authorization),
            "TPM2_CreatePrimary");
        if (!response || response->size() < 4) {
            co_return std::unexpected(response ?
                                          failure(errc::corrupt,
                                                  "invalid TPM primary response",
                                                  "TPM2_CreatePrimary") :
                                          response.error());
        }
        co_return get_u32(*response, 0);
    }

    auto start_policy_session() -> runtime::task<std::uint32_t> {
        auto body = std::vector<std::byte>{};
        put_u32(body, tpm_rh_null);
        put_u32(body, tpm_rh_null);
        put_u16(body, 0);
        put_u16(body, 0);
        body.push_back(std::byte{0x01});
        put_u16(body, 0x0010);
        put_u16(body, 0x000b);
        auto response = co_await submit(
            command(tpm_st_no_sessions, tpm2_start_auth_session, body),
            "TPM2_StartAuthSession");
        if (!response || response->size() < 6) {
            co_return std::unexpected(response ?
                                          failure(errc::corrupt,
                                                  "invalid TPM policy-session response",
                                                  "TPM2_StartAuthSession") :
                                          response.error());
        }
        co_return get_u32(*response, 0);
    }

    auto apply_pcr_policy(std::uint32_t session) -> runtime::task<void> {
        auto body = std::vector<std::byte>{};
        put_u32(body, session);
        auto selection = pcr_selection(pcrs_);
        body.insert(body.end(), selection.begin(), selection.end());
        auto response = co_await submit(
            command(tpm_st_no_sessions, tpm2_policy_pcr, body), "TPM2_PolicyPCR");
        if (!response) {
            co_return std::unexpected(response.error());
        }
        co_return {};
    }

    auto policy_digest(std::uint32_t session) -> runtime::task<std::vector<std::byte>> {
        auto body = std::vector<std::byte>{};
        put_u32(body, session);
        auto response = co_await submit(
            command(tpm_st_no_sessions, tpm2_policy_get_digest, body),
            "TPM2_PolicyGetDigest");
        if (!response) {
            co_return std::unexpected(response.error());
        }
        auto offset = std::size_t{};
        auto digest = read_sized(*response, offset);
        if (!digest) {
            co_return std::unexpected(digest.error());
        }
        co_return std::vector<std::byte>{digest->begin(), digest->end()};
    }

    auto flush(std::uint32_t handle) -> runtime::task<void> {
        auto body = std::vector<std::byte>{};
        put_u32(body, handle);
        auto response = co_await submit(
            command(tpm_st_no_sessions, tpm2_flush_context, body), "TPM2_FlushContext");
        if (!response) {
            co_return std::unexpected(response.error());
        }
        co_return {};
    }

    std::string parameters_;
    std::vector<std::uint8_t> pcrs_;
    std::shared_ptr<tpm_transport> transport_;
};

} // namespace

auto make_tpm_provider(std::string parameters) -> result<std::shared_ptr<key_provider>> {
    auto pcrs = parse_pcrs(parameters);
    if (!pcrs) {
        return std::unexpected(pcrs.error());
    }
    return std::make_shared<tpm_provider>(
        std::move(parameters), std::move(*pcrs), selected_transport());
}

void install_tpm_transport(std::shared_ptr<tpm_transport> transport) {
    auto lock = std::scoped_lock{transport_mutex()};
    installed_transport() = std::move(transport);
}

} // namespace liquid::custody_detail
