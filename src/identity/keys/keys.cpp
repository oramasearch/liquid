#include "identity/keys/backend.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>
#include <utility>

#if defined(LIQUID_HAVE_PCSC)
#if defined(__APPLE__)
#include <PCSC/winscard.h>
#include <PCSC/wintypes.h>
#elif defined(__linux__)
#include <PCSC/winscard.h>
#include <PCSC/wintypes.h>
#else
#include <winscard.h>
#endif
#endif

#include "crypto/crypto.hpp"
#include "crypto/signature.hpp"
#include "encoding/cbor.hpp"
#include "monocypher-ed25519.h"
#include "monocypher.h"

namespace liquid::identity_detail {
namespace {

namespace cbor = encoding::cbor;

auto failure(errc code, std::string message, std::string context) -> error {
    return {code, std::move(message), std::move(context)};
}

auto byte_string(std::span<const std::byte> input) -> cbor::value {
    return cbor::value{cbor::value::bytes{input.begin(), input.end()}};
}

auto field(const cbor::value& source, std::string_view name) -> const cbor::value* {
    const auto* values = std::get_if<cbor::value::map>(&source.data);
    if (!values) {
        return nullptr;
    }
    for (const auto& [key, value] : *values) {
        const auto* text = std::get_if<std::string>(&key.data);
        if (text && *text == name) {
            return &value;
        }
    }
    return nullptr;
}

auto field(const cbor::value& source, std::int64_t name) -> const cbor::value* {
    const auto* values = std::get_if<cbor::value::map>(&source.data);
    if (!values) {
        return nullptr;
    }
    for (const auto& [key, value] : *values) {
        if (const auto* signed_key = std::get_if<std::int64_t>(&key.data);
            signed_key && *signed_key == name) {
            return &value;
        }
        if (const auto* unsigned_key = std::get_if<std::uint64_t>(&key.data);
            unsigned_key && name >= 0 && *unsigned_key == static_cast<std::uint64_t>(name)) {
            return &value;
        }
    }
    return nullptr;
}

class software_backend final : public key_backend {
public:
    explicit software_backend(std::span<const std::byte> secret) {
        std::ranges::copy(secret, secret_.begin());
        std::ranges::copy(secret_.begin() + 32, secret_.end(), public_key_.begin());
    }

    ~software_backend() override {
        crypto_wipe(secret_.data(), secret_.size());
    }

    auto algorithm() const noexcept -> std::string_view override {
        return "ed25519";
    }

    auto custody() const noexcept -> std::string_view override {
        return "software";
    }

    auto public_key() const noexcept -> std::span<const std::byte> override {
        return public_key_;
    }

    auto descriptor() const -> std::vector<std::byte> override {
        return {secret_.begin(), secret_.end()};
    }

    auto sign(std::vector<std::byte> input) -> runtime::task<backend_signature> override {
        auto bytes = std::vector<std::byte>(64);
        crypto_ed25519_sign(reinterpret_cast<std::uint8_t*>(bytes.data()),
                            reinterpret_cast<const std::uint8_t*>(secret_.data()),
                            reinterpret_cast<const std::uint8_t*>(input.data()),
                            input.size());
        crypto_wipe(input.data(), input.size());
        co_return backend_signature{"ed25519", std::move(bytes), {}, {}};
    }

private:
    std::array<std::byte, 64> secret_{};
    std::array<std::byte, 32> public_key_{};
};

class unavailable_piv_transport final : public piv_transport {
public:
    auto transmit(std::span<const std::byte>) -> result<std::vector<std::byte>> override {
        return std::unexpected(failure(errc::unsupported,
                                       "no PIV smart-card reader is available",
                                       "PIV APDUs over operating-system PC/SC"));
    }
};

#if defined(LIQUID_HAVE_PCSC)
class pcsc_piv_transport final : public piv_transport {
public:
    ~pcsc_piv_transport() override {
        if (card_ != 0) {
            SCardDisconnect(card_, SCARD_LEAVE_CARD);
        }
        if (context_ != 0) {
            SCardReleaseContext(context_);
        }
    }

    auto transmit(std::span<const std::byte> apdu) -> result<std::vector<std::byte>> override {
        auto lock = std::scoped_lock{mutex_};
        auto connected = connect();
        if (!connected) {
            return std::unexpected(connected.error());
        }
        const auto* send_pci = protocol_ == SCARD_PROTOCOL_T0 ? SCARD_PCI_T0 : SCARD_PCI_T1;
        auto output = std::vector<std::byte>(4096);
        auto output_size = static_cast<DWORD>(output.size());
        const auto status = SCardTransmit(card_,
                                          send_pci,
                                          reinterpret_cast<const BYTE*>(apdu.data()),
                                          static_cast<DWORD>(apdu.size()),
                                          nullptr,
                                          reinterpret_cast<BYTE*>(output.data()),
                                          &output_size);
        if (status != SCARD_S_SUCCESS) {
            return std::unexpected(failure(
                errc::io, "PC/SC transmit failed", std::to_string(static_cast<long>(status))));
        }
        output.resize(output_size);
        return output;
    }

private:
    auto connect() -> result<void> {
        if (card_ != 0) {
            return {};
        }
        auto status = SCardEstablishContext(SCARD_SCOPE_SYSTEM, nullptr, nullptr, &context_);
        if (status != SCARD_S_SUCCESS) {
            return std::unexpected(failure(errc::unsupported,
                                           "PC/SC service is unavailable",
                                           std::to_string(static_cast<long>(status))));
        }
        auto reader_size = DWORD{};
        status = SCardListReaders(context_, nullptr, nullptr, &reader_size);
        if (status != SCARD_S_SUCCESS || reader_size <= 1) {
            return std::unexpected(
                failure(errc::unsupported, "no PIV smart-card reader is available", "PC/SC"));
        }
        auto readers = std::vector<char>(reader_size);
        status = SCardListReaders(context_, nullptr, readers.data(), &reader_size);
        if (status != SCARD_S_SUCCESS) {
            return std::unexpected(failure(errc::io,
                                           "cannot enumerate PC/SC readers",
                                           std::to_string(static_cast<long>(status))));
        }
        status = SCardConnect(context_,
                              readers.data(),
                              SCARD_SHARE_SHARED,
                              SCARD_PROTOCOL_T0 | SCARD_PROTOCOL_T1,
                              &card_,
                              &protocol_);
        if (status != SCARD_S_SUCCESS) {
            card_ = 0;
            return std::unexpected(failure(errc::unsupported,
                                           "no PIV card is available",
                                           std::to_string(static_cast<long>(status))));
        }
        return {};
    }

    std::mutex mutex_;
    SCARDCONTEXT context_{};
    SCARDHANDLE card_{};
    DWORD protocol_{};
};
#endif

auto transport_mutex() -> std::mutex& {
    static auto value = std::mutex{};
    return value;
}

auto fido_transport() -> std::shared_ptr<fido2_transport>& {
    static auto value = std::shared_ptr<fido2_transport>{};
    return value;
}

auto piv_device() -> std::shared_ptr<piv_transport>& {
    static auto value = std::shared_ptr<piv_transport>{};
    return value;
}

auto selected_fido_transport() -> std::shared_ptr<fido2_transport> {
    auto lock = std::scoped_lock{transport_mutex()};
    if (fido_transport()) {
        return fido_transport();
    }
    return operating_system_fido2_transport();
}

auto selected_piv_transport() -> std::shared_ptr<piv_transport> {
    auto lock = std::scoped_lock{transport_mutex()};
    if (piv_device()) {
        return piv_device();
    }
#if defined(LIQUID_HAVE_PCSC)
    return std::make_shared<pcsc_piv_transport>();
#else
    return std::make_shared<unavailable_piv_transport>();
#endif
}

class fido2_backend final : public key_backend {
public:
    fido2_backend(std::shared_ptr<fido2_transport> transport,
                  std::string algorithm,
                  std::vector<std::byte> credential,
                  std::vector<std::byte> public_key)
        : transport_(std::move(transport)), algorithm_(std::move(algorithm)),
          credential_(std::move(credential)), public_key_(std::move(public_key)) {}

    auto algorithm() const noexcept -> std::string_view override {
        return algorithm_;
    }

    auto custody() const noexcept -> std::string_view override {
        return "fido2";
    }

    auto public_key() const noexcept -> std::span<const std::byte> override {
        return public_key_;
    }

    auto descriptor() const -> std::vector<std::byte> override {
        auto encoded = cbor::encode(cbor::value{cbor::value::map{
            {"algorithm", algorithm_},
            {"credential_id", byte_string(credential_)},
            {"public_key", byte_string(public_key_)},
        }});
        return encoded.value_or(std::vector<std::byte>{});
    }

    auto sign(std::vector<std::byte> input) -> runtime::task<backend_signature> override {
        auto client_data = crypto::digest(crypto::digest_kind::sha256, input);
        crypto_wipe(input.data(), input.size());
        if (!client_data) {
            co_return std::unexpected(client_data.error());
        }
        auto allow = cbor::value::array{cbor::value{cbor::value::map{
            {"type", "public-key"},
            {"id", byte_string(credential_)},
        }}};
        auto request = cbor::encode(cbor::value{cbor::value::map{
            {cbor::value{std::uint64_t{1}}, "liquid"},
            {cbor::value{std::uint64_t{2}}, byte_string(*client_data)},
            {cbor::value{std::uint64_t{3}}, cbor::value{std::move(allow)}},
        }});
        if (!request) {
            co_return std::unexpected(request.error());
        }
        auto response = co_await runtime::offload(
            [transport = transport_, request = std::move(*request)]() mutable {
                return transport->transact(0x02, request);
            });
        if (!response) {
            co_return std::unexpected(response.error());
        }
        if (!*response) {
            co_return std::unexpected(response->error());
        }
        auto decoded = cbor::decode(**response);
        if (!decoded) {
            co_return std::unexpected(decoded.error());
        }
        const auto* authenticator = field(*decoded, 2);
        const auto* signature = field(*decoded, 3);
        const auto* authenticator_bytes =
            authenticator ? std::get_if<cbor::value::bytes>(&authenticator->data) : nullptr;
        const auto* signature_bytes =
            signature ? std::get_if<cbor::value::bytes>(&signature->data) : nullptr;
        if (!authenticator_bytes || authenticator_bytes->empty() || !signature_bytes ||
            signature_bytes->empty()) {
            co_return std::unexpected(
                failure(errc::corrupt, "invalid authenticatorGetAssertion response", "FIDO2"));
        }
        co_return backend_signature{
            algorithm_, *signature_bytes, *authenticator_bytes, std::move(*client_data)};
    }

private:
    std::shared_ptr<fido2_transport> transport_;
    std::string algorithm_;
    std::vector<std::byte> credential_;
    std::vector<std::byte> public_key_;
};

auto slot_byte(std::string_view slot) -> result<std::byte> {
    if (slot != "9c") {
        return std::unexpected(
            failure(errc::invalid_argument, "PIV signing requires slot 9c", std::string{slot}));
    }
    return std::byte{0x9c};
}

auto tlv_value(std::span<const std::byte> encoded, std::uint16_t expected_tag)
    -> result<std::span<const std::byte>> {
    if (encoded.size() < 2) {
        return std::unexpected(failure(errc::corrupt, "truncated PIV TLV", "PIV"));
    }
    auto offset = std::size_t{};
    auto tag = static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(encoded[offset++]));
    if ((tag & 0x1fU) == 0x1fU) {
        if (offset == encoded.size()) {
            return std::unexpected(failure(errc::corrupt, "truncated PIV TLV tag", "PIV"));
        }
        tag = static_cast<std::uint16_t>((tag << 8U) |
                                         std::to_integer<std::uint8_t>(encoded[offset++]));
    }
    if (tag != expected_tag || offset == encoded.size()) {
        return std::unexpected(failure(errc::corrupt, "unexpected PIV TLV tag", "PIV"));
    }
    auto size = static_cast<std::size_t>(std::to_integer<std::uint8_t>(encoded[offset++]));
    if (size == 0x81U) {
        if (offset == encoded.size()) {
            return std::unexpected(failure(errc::corrupt, "truncated PIV TLV length", "PIV"));
        }
        size = std::to_integer<std::uint8_t>(encoded[offset++]);
    } else if (size == 0x82U) {
        if (offset + 2 > encoded.size()) {
            return std::unexpected(failure(errc::corrupt, "truncated PIV TLV length", "PIV"));
        }
        size = (static_cast<std::size_t>(std::to_integer<std::uint8_t>(encoded[offset])) << 8U) |
               std::to_integer<std::uint8_t>(encoded[offset + 1]);
        offset += 2;
    } else if (size >= 0x80U) {
        return std::unexpected(failure(errc::corrupt, "unsupported PIV TLV length", "PIV"));
    }
    if (offset + size != encoded.size()) {
        return std::unexpected(failure(errc::corrupt, "invalid PIV TLV length", "PIV"));
    }
    return encoded.subspan(offset, size);
}

auto piv_response(std::vector<std::byte> response, std::string_view operation)
    -> result<std::vector<std::byte>> {
    if (response.size() < 2 || response.end()[-2] != std::byte{0x90} ||
        response.back() != std::byte{0x00}) {
        return std::unexpected(
            failure(errc::unauthorized, "PIV APDU failed", std::string{operation}));
    }
    response.resize(response.size() - 2);
    return response;
}

class piv_backend final : public key_backend {
public:
    piv_backend(std::shared_ptr<piv_transport> transport,
                std::string slot,
                std::vector<std::byte> public_key)
        : transport_(std::move(transport)), slot_(std::move(slot)),
          public_key_(std::move(public_key)) {}

    auto algorithm() const noexcept -> std::string_view override {
        return "ecdsa-p256";
    }

    auto custody() const noexcept -> std::string_view override {
        return custody_;
    }

    auto public_key() const noexcept -> std::span<const std::byte> override {
        return public_key_;
    }

    auto descriptor() const -> std::vector<std::byte> override {
        auto encoded = cbor::encode(cbor::value{cbor::value::map{
            {"slot", slot_},
            {"public_key", byte_string(public_key_)},
        }});
        return encoded.value_or(std::vector<std::byte>{});
    }

    auto sign(std::vector<std::byte> input) -> runtime::task<backend_signature> override {
        auto digest = crypto::digest(crypto::digest_kind::sha256, input);
        crypto_wipe(input.data(), input.size());
        if (!digest) {
            co_return std::unexpected(digest.error());
        }
        auto slot = slot_byte(slot_);
        if (!slot) {
            co_return std::unexpected(slot.error());
        }
        auto apdu = std::vector<std::byte>{std::byte{0x00},
                                           std::byte{0x87},
                                           std::byte{0x11},
                                           *slot,
                                           std::byte{0x26},
                                           std::byte{0x7c},
                                           std::byte{0x24},
                                           std::byte{0x82},
                                           std::byte{0x00},
                                           std::byte{0x81},
                                           std::byte{0x20}};
        apdu.insert(apdu.end(), digest->begin(), digest->end());
        apdu.push_back(std::byte{0x00});
        auto response =
            co_await runtime::offload([transport = transport_, apdu = std::move(apdu)]() mutable {
                return transport->transmit(apdu);
            });
        if (!response) {
            co_return std::unexpected(response.error());
        }
        if (!*response) {
            co_return std::unexpected(response->error());
        }
        auto data = piv_response(std::move(response->value()), custody_);
        if (!data) {
            co_return std::unexpected(data.error());
        }
        auto dynamic_authentication = tlv_value(*data, 0x7c);
        if (!dynamic_authentication) {
            co_return std::unexpected(dynamic_authentication.error());
        }
        auto signature_value = tlv_value(*dynamic_authentication, 0x82);
        if (!signature_value || signature_value->empty()) {
            co_return std::unexpected(signature_value
                                          ? failure(errc::corrupt, "empty PIV signature", custody_)
                                          : signature_value.error());
        }
        auto signature = std::vector<std::byte>{signature_value->begin(), signature_value->end()};
        co_return backend_signature{"ecdsa-p256", std::move(signature), {}, {}};
    }

private:
    std::shared_ptr<piv_transport> transport_;
    std::string slot_;
    std::vector<std::byte> public_key_;
    std::string custody_{"piv:9c"};
};

auto decode_fido_descriptor(std::span<const std::byte> descriptor,
                            std::shared_ptr<fido2_transport> transport)
    -> result<std::unique_ptr<key_backend>> {
    auto decoded = cbor::decode(descriptor);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    const auto* algorithm = field(*decoded, "algorithm");
    const auto* credential = field(*decoded, "credential_id");
    const auto* public_key = field(*decoded, "public_key");
    const auto* algorithm_text = algorithm ? std::get_if<std::string>(&algorithm->data) : nullptr;
    const auto* credential_bytes =
        credential ? std::get_if<cbor::value::bytes>(&credential->data) : nullptr;
    const auto* public_bytes =
        public_key ? std::get_if<cbor::value::bytes>(&public_key->data) : nullptr;
    if (!algorithm_text || (*algorithm_text != "fido2-eddsa" && *algorithm_text != "fido2-es256") ||
        !credential_bytes || credential_bytes->empty() || !public_bytes ||
        ((*algorithm_text == "fido2-eddsa" && public_bytes->size() != 32) ||
         (*algorithm_text == "fido2-es256" && public_bytes->size() != 64))) {
        return std::unexpected(
            failure(errc::corrupt, "invalid FIDO2 credential descriptor", "identity key"));
    }
    return std::make_unique<fido2_backend>(
        std::move(transport), *algorithm_text, *credential_bytes, *public_bytes);
}

auto decode_make_credential(std::span<const std::byte> response,
                            std::shared_ptr<fido2_transport> transport)
    -> result<std::unique_ptr<key_backend>> {
    auto decoded = cbor::decode(response);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    const auto* authenticator = field(*decoded, 2);
    const auto* authenticator_bytes =
        authenticator ? std::get_if<cbor::value::bytes>(&authenticator->data) : nullptr;
    if (!authenticator_bytes || authenticator_bytes->size() < 55 ||
        (std::to_integer<std::uint8_t>((*authenticator_bytes)[32]) & 0x40U) == 0) {
        return std::unexpected(
            failure(errc::corrupt, "invalid authenticatorMakeCredential response", "FIDO2"));
    }
    auto offset = std::size_t{53};
    const auto credential_size =
        (static_cast<std::size_t>(std::to_integer<std::uint8_t>((*authenticator_bytes)[offset]))
         << 8U) |
        std::to_integer<std::uint8_t>((*authenticator_bytes)[offset + 1]);
    offset += 2;
    if (credential_size == 0 || offset + credential_size >= authenticator_bytes->size()) {
        return std::unexpected(
            failure(errc::corrupt, "invalid FIDO2 credential identifier", "FIDO2"));
    }
    auto credential =
        std::vector<std::byte>{authenticator_bytes->begin() + offset,
                               authenticator_bytes->begin() + offset + credential_size};
    offset += credential_size;
    auto cose = cbor::decode(std::span{*authenticator_bytes}.subspan(offset));
    if (!cose) {
        return std::unexpected(cose.error());
    }
    const auto* algorithm_value = field(*cose, 3);
    const auto* algorithm =
        algorithm_value ? std::get_if<std::int64_t>(&algorithm_value->data) : nullptr;
    const auto* x_value = field(*cose, -2);
    const auto* x = x_value ? std::get_if<cbor::value::bytes>(&x_value->data) : nullptr;
    if (!algorithm || !x || x->size() != 32) {
        return std::unexpected(failure(errc::corrupt, "invalid FIDO2 COSE key", "FIDO2"));
    }
    if (*algorithm == -8) {
        return std::make_unique<fido2_backend>(
            std::move(transport), "fido2-eddsa", std::move(credential), *x);
    }
    const auto* y_value = field(*cose, -3);
    const auto* y = y_value ? std::get_if<cbor::value::bytes>(&y_value->data) : nullptr;
    if (*algorithm != -7 || !y || y->size() != 32) {
        return std::unexpected(failure(errc::corrupt, "unsupported FIDO2 COSE key", "FIDO2"));
    }
    auto public_key = *x;
    public_key.insert(public_key.end(), y->begin(), y->end());
    return std::make_unique<fido2_backend>(
        std::move(transport), "fido2-es256", std::move(credential), std::move(public_key));
}

auto decode_piv_descriptor(std::span<const std::byte> descriptor,
                           std::shared_ptr<piv_transport> transport)
    -> result<std::unique_ptr<key_backend>> {
    auto decoded = cbor::decode(descriptor);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    const auto* slot = field(*decoded, "slot");
    const auto* public_key = field(*decoded, "public_key");
    const auto* slot_text = slot ? std::get_if<std::string>(&slot->data) : nullptr;
    const auto* public_bytes =
        public_key ? std::get_if<cbor::value::bytes>(&public_key->data) : nullptr;
    if (!slot_text || *slot_text != "9c" || !public_bytes || public_bytes->size() != 64) {
        return std::unexpected(
            failure(errc::corrupt, "invalid PIV key descriptor", "identity key"));
    }
    return std::make_unique<piv_backend>(std::move(transport), *slot_text, *public_bytes);
}

} // namespace

auto make_software_backend() -> result<std::unique_ptr<key_backend>> {
    auto seed = std::array<std::byte, 32>{};
    auto random = crypto_detail::random_bytes(seed);
    if (!random) {
        return std::unexpected(random.error());
    }
    auto secret = std::array<std::byte, 64>{};
    auto public_key = std::array<std::byte, 32>{};
    crypto_ed25519_key_pair(reinterpret_cast<std::uint8_t*>(secret.data()),
                            reinterpret_cast<std::uint8_t*>(public_key.data()),
                            reinterpret_cast<std::uint8_t*>(seed.data()));
    crypto_wipe(seed.data(), seed.size());
    auto output = std::make_unique<software_backend>(secret);
    crypto_wipe(secret.data(), secret.size());
    return output;
}

auto make_software_backend(std::span<const std::byte> secret)
    -> result<std::unique_ptr<key_backend>> {
    if (secret.size() != 64) {
        return std::unexpected(
            failure(errc::corrupt, "invalid software identity key", "identity key"));
    }
    return std::make_unique<software_backend>(secret);
}

auto make_fido2_backend() -> runtime::task<std::unique_ptr<key_backend>> {
    auto transport = selected_fido_transport();
    auto client_data = std::array<std::byte, 32>{};
    auto random = crypto_detail::random_bytes(client_data);
    if (!random) {
        co_return std::unexpected(random.error());
    }
    auto user_id = std::array<std::byte, 32>{};
    random = crypto_detail::random_bytes(user_id);
    if (!random) {
        co_return std::unexpected(random.error());
    }
    auto algorithms = cbor::value::array{
        cbor::value{
            cbor::value::map{{"type", "public-key"}, {"alg", cbor::value{std::int64_t{-8}}}}},
        cbor::value{
            cbor::value::map{{"type", "public-key"}, {"alg", cbor::value{std::int64_t{-7}}}}},
    };
    auto request = cbor::encode(cbor::value{cbor::value::map{
        {cbor::value{std::uint64_t{1}}, byte_string(client_data)},
        {cbor::value{std::uint64_t{2}},
         cbor::value{cbor::value::map{{"id", "liquid"}, {"name", "liquid"}}}},
        {cbor::value{std::uint64_t{3}},
         cbor::value{cbor::value::map{{"id", byte_string(user_id)},
                                      {"name", "liquid-device"},
                                      {"displayName", "liquid device identity"}}}},
        {cbor::value{std::uint64_t{4}}, cbor::value{std::move(algorithms)}},
        {cbor::value{std::uint64_t{6}},
         cbor::value{cbor::value::map{{"hmac-secret", cbor::value{true}}}}},
    }});
    if (!request) {
        co_return std::unexpected(request.error());
    }
    auto response = co_await runtime::offload([transport, request = std::move(*request)]() mutable {
        return transport->transact(0x01, request);
    });
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (!*response) {
        co_return std::unexpected(response->error());
    }
    co_return decode_make_credential(**response, std::move(transport));
}

auto make_piv_backend(std::string slot) -> runtime::task<std::unique_ptr<key_backend>> {
    auto parsed_slot = slot_byte(slot);
    if (!parsed_slot) {
        co_return std::unexpected(parsed_slot.error());
    }
    auto transport = selected_piv_transport();
    constexpr std::array<std::byte, 15> select{std::byte{0x00},
                                               std::byte{0xa4},
                                               std::byte{0x04},
                                               std::byte{0x00},
                                               std::byte{0x09},
                                               std::byte{0xa0},
                                               std::byte{0x00},
                                               std::byte{0x00},
                                               std::byte{0x03},
                                               std::byte{0x08},
                                               std::byte{0x00},
                                               std::byte{0x00},
                                               std::byte{0x10},
                                               std::byte{0x00},
                                               std::byte{0x00}};
    auto selected =
        co_await runtime::offload([transport, select] { return transport->transmit(select); });
    if (!selected) {
        co_return std::unexpected(selected.error());
    }
    if (!*selected) {
        co_return std::unexpected(selected->error());
    }
    auto selection = piv_response(std::move(selected->value()), "PIV application selection");
    if (!selection) {
        co_return std::unexpected(selection.error());
    }
    const auto generate = std::array<std::byte, 11>{std::byte{0x00},
                                                    std::byte{0x47},
                                                    std::byte{0x00},
                                                    *parsed_slot,
                                                    std::byte{0x05},
                                                    std::byte{0xac},
                                                    std::byte{0x03},
                                                    std::byte{0x80},
                                                    std::byte{0x01},
                                                    std::byte{0x11},
                                                    std::byte{0x00}};
    auto generated =
        co_await runtime::offload([transport, generate] { return transport->transmit(generate); });
    if (!generated) {
        co_return std::unexpected(generated.error());
    }
    if (!*generated) {
        co_return std::unexpected(generated->error());
    }
    auto response = piv_response(std::move(generated->value()), "PIV key generation");
    if (!response) {
        co_return std::unexpected(response.error());
    }
    auto public_template = tlv_value(*response, 0x7f49);
    if (!public_template) {
        co_return std::unexpected(public_template.error());
    }
    auto public_point = tlv_value(*public_template, 0x86);
    if (!public_point || public_point->size() != 65 || public_point->front() != std::byte{0x04}) {
        co_return std::unexpected(
            public_point ? failure(errc::corrupt, "invalid PIV P-256 public key", "piv:" + slot)
                         : public_point.error());
    }
    auto public_key = std::vector<std::byte>{public_point->begin() + 1, public_point->end()};
    co_return std::make_unique<piv_backend>(
        std::move(transport), std::move(slot), std::move(public_key));
}

auto open_hardware_backend(std::string custody, std::span<const std::byte> descriptor)
    -> result<std::unique_ptr<key_backend>> {
    if (custody == "fido2") {
        return decode_fido_descriptor(descriptor, selected_fido_transport());
    }
    if (custody == "piv:9c") {
        return decode_piv_descriptor(descriptor, selected_piv_transport());
    }
    return std::unexpected(
        failure(errc::invalid_argument, "unsupported identity key custody", std::move(custody)));
}

void install_fido2_transport(std::shared_ptr<fido2_transport> transport) {
    auto lock = std::scoped_lock{transport_mutex()};
    fido_transport() = std::move(transport);
}

void install_piv_transport(std::shared_ptr<piv_transport> transport) {
    auto lock = std::scoped_lock{transport_mutex()};
    piv_device() = std::move(transport);
}

} // namespace liquid::identity_detail
