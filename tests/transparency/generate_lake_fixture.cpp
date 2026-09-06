#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ts.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "lake/transparency/timestamps.hpp"

namespace {

template <class T, auto Free> struct deleter {
    auto operator()(T* value) const -> void {
        Free(value);
    }
};

using key = std::unique_ptr<EVP_PKEY, deleter<EVP_PKEY, EVP_PKEY_free>>;
using certificate = std::unique_ptr<X509, deleter<X509, X509_free>>;
using response_context = std::unique_ptr<TS_RESP_CTX, deleter<TS_RESP_CTX, TS_RESP_CTX_free>>;
using response = std::unique_ptr<TS_RESP, deleter<TS_RESP, TS_RESP_free>>;
using bio = std::unique_ptr<BIO, deleter<BIO, BIO_free>>;

auto p256_key() -> key {
    EVP_PKEY_CTX* context = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    EVP_PKEY* output = nullptr;
    if (context == nullptr || EVP_PKEY_keygen_init(context) != 1 ||
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(context, NID_X9_62_prime256v1) != 1 ||
        EVP_PKEY_keygen(context, &output) != 1) {
        EVP_PKEY_free(output);
        output = nullptr;
    }
    EVP_PKEY_CTX_free(context);
    return key(output);
}

auto add_extension(X509* value, X509* issuer, int identifier, const char* contents) -> bool {
    X509V3_CTX context{};
    X509V3_set_ctx_nodb(&context);
    X509V3_set_ctx(&context, issuer, value, nullptr, nullptr, 0);
    X509_EXTENSION* extension =
        X509V3_EXT_conf_nid(nullptr, &context, identifier, const_cast<char*>(contents));
    if (extension == nullptr) {
        return false;
    }
    const bool inserted = X509_add_ext(value, extension, -1) == 1;
    X509_EXTENSION_free(extension);
    return inserted;
}

auto make_certificate(EVP_PKEY* public_key,
                      X509* issuer,
                      EVP_PKEY* issuer_key,
                      long serial,
                      const char* common_name,
                      bool authority) -> certificate {
    certificate output(X509_new());
    if (!output || X509_set_version(output.get(), 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(output.get()), serial) != 1 ||
        !X509_gmtime_adj(X509_getm_notBefore(output.get()), -60) ||
        !X509_gmtime_adj(X509_getm_notAfter(output.get()), 400L * 24 * 60 * 60) ||
        X509_set_pubkey(output.get(), public_key) != 1) {
        return {};
    }
    X509_NAME* subject = X509_get_subject_name(output.get());
    if (X509_NAME_add_entry_by_txt(subject,
                                   "CN",
                                   MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>(common_name),
                                   -1,
                                   -1,
                                   0) != 1 ||
        X509_set_issuer_name(output.get(),
                             issuer == nullptr ? subject : X509_get_subject_name(issuer)) != 1) {
        return {};
    }
    const bool extensions =
        authority
            ? add_extension(
                  output.get(), output.get(), NID_basic_constraints, "critical,CA:TRUE") &&
                  add_extension(
                      output.get(), output.get(), NID_key_usage, "critical,keyCertSign,cRLSign")
            : add_extension(output.get(), issuer, NID_basic_constraints, "critical,CA:FALSE") &&
                  add_extension(output.get(), issuer, NID_key_usage, "critical,digitalSignature") &&
                  add_extension(output.get(), issuer, NID_ext_key_usage, "critical,timeStamping");
    if (!extensions || X509_sign(output.get(), issuer_key, EVP_sha256()) <= 0) {
        return {};
    }
    return output;
}

class stub_tsa {
public:
    stub_tsa() {
        root_key_ = p256_key();
        signer_key_ = p256_key();
        root_ = make_certificate(
            root_key_.get(), nullptr, root_key_.get(), 1, "Lake fixture TSA root", true);
        signer_ = make_certificate(
            signer_key_.get(), root_.get(), root_key_.get(), 2, "Lake fixture TSA", false);
        bio output(BIO_new(BIO_s_mem()));
        if (output && root_ && PEM_write_bio_X509(output.get(), root_.get()) == 1) {
            BUF_MEM* memory = nullptr;
            BIO_get_mem_ptr(output.get(), &memory);
            trust_pem_.assign(memory->data, memory->length);
        }
    }

    auto send(std::string_view, std::string_view request) -> lake::transparency::TsaReply {
        response_context context(TS_RESP_CTX_new());
        ASN1_OBJECT* policy = OBJ_txt2obj("1.3.6.1.4.1.57264.1", 1);
        bio input(BIO_new_mem_buf(request.data(), static_cast<int>(request.size())));
        if (!context || policy == nullptr || !input || !root_ || !signer_ ||
            TS_RESP_CTX_set_signer_cert(context.get(), signer_.get()) != 1 ||
            TS_RESP_CTX_set_signer_key(context.get(), signer_key_.get()) != 1 ||
            TS_RESP_CTX_set_signer_digest(context.get(), EVP_sha256()) != 1 ||
            TS_RESP_CTX_set_def_policy(context.get(), policy) != 1 ||
            TS_RESP_CTX_add_md(context.get(), EVP_sha256()) != 1) {
            ASN1_OBJECT_free(policy);
            return {500, {}, "could not configure fixture TSA"};
        }
        ASN1_OBJECT_free(policy);
        response generated(TS_RESP_create_response(context.get(), input.get()));
        if (!generated) {
            return {500, {}, "could not create fixture TSA response"};
        }
        const int size = i2d_TS_RESP(generated.get(), nullptr);
        if (size <= 0) {
            return {500, {}, "could not encode fixture TSA response"};
        }
        std::string body(static_cast<std::size_t>(size), '\0');
        unsigned char* cursor = reinterpret_cast<unsigned char*>(body.data());
        if (i2d_TS_RESP(generated.get(), &cursor) != size) {
            return {500, {}, "could not encode fixture TSA response"};
        }
        return {200, std::move(body), {}};
    }

    [[nodiscard]] auto trust_pem() const -> const std::string& {
        return trust_pem_;
    }

private:
    key root_key_;
    key signer_key_;
    certificate root_;
    certificate signer_;
    std::string trust_pem_;
};

auto write_bytes(const std::filesystem::path& path, std::string_view value) -> bool {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    return static_cast<bool>(output);
}

} // namespace

auto main(int argc, char** argv) -> int {
    if (argc != 2) {
        return 2;
    }
    const std::filesystem::path output = argv[1];
    const std::filesystem::path store = output / "generator-store";
    std::filesystem::create_directories(output);

    stub_tsa tsa;
    if (tsa.trust_pem().empty()) {
        return 1;
    }
    const std::string checkpoint_digest(64, '1');
    const std::string checkpoint_signature = "lake issue 210 checkpoint signature value";
    const std::string signature_base64 = "bGFrZSBpc3N1ZSAyMTAgY2hlY2twb2ludCBzaWduYXR1cmUgdmFsdWU=";
    lake::transparency::TimestampService service(store, "fixture");
    const auto sender = [&tsa](std::string_view endpoint, std::string_view request) {
        return tsa.send(endpoint, request);
    };
    const auto initial = service.timestamp(checkpoint_digest,
                                           signature_base64,
                                           "https://tsa.example.test/rfc3161",
                                           tsa.trust_pem(),
                                           {},
                                           sender);
    if (!initial) {
        return 1;
    }
    const auto generated = service.directory() / (checkpoint_digest + ".ers");
    std::filesystem::copy_file(
        generated, output / "lake-initial.ers", std::filesystem::copy_options::overwrite_existing);
    const auto renewed = service.renew(
        checkpoint_digest, "https://tsa.example.test/rfc3161", tsa.trust_pem(), {}, sender);
    if (!renewed) {
        return 1;
    }
    std::filesystem::copy_file(
        generated, output / "lake-renewed.ers", std::filesystem::copy_options::overwrite_existing);
    if (!write_bytes(output / "tsa-root.pem", tsa.trust_pem()) ||
        !write_bytes(output / "checkpoint-signature.bin", checkpoint_signature)) {
        return 1;
    }
    std::filesystem::remove_all(store);
    return 0;
}
