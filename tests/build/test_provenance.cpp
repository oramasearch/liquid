#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include "../temporary_directory.hpp"
#include "build/provenance.hpp"
#include "build/repository_access.hpp"
#include "formats/json.hpp"
#include "liquid/build.hpp"
#include "liquid/repository.hpp"
#include "objects/records.hpp"

namespace {

class temporary_repository {
public:
    temporary_repository()
        : path_(liquid::test::fixture_path("liquid-provenance-test-" + std::to_string(next_++))) {
        auto code = std::error_code{};
        std::filesystem::remove_all(path_, code);
    }

    ~temporary_repository() {
        auto code = std::error_code{};
        std::filesystem::remove_all(path_, code);
    }

    auto path() const -> const std::filesystem::path& {
        return path_;
    }

private:
    inline static std::atomic_uint64_t next_{};
    std::filesystem::path path_;
};

auto example_digest(unsigned char value) -> liquid::digest {
    auto output = liquid::digest{liquid::algorithm::blake3, {}};
    output.bytes.front() = std::byte{value};
    return output;
}

auto digest_hex(const liquid::digest& value) -> std::string {
    return liquid::to_string(value).substr(std::string_view{"blake3:"}.size());
}

auto field(const liquid::formats::json::value& value, std::string_view name)
    -> const liquid::formats::json::value* {
    const auto* object = std::get_if<liquid::formats::json::value::object>(&value.data);
    if (!object) {
        return nullptr;
    }
    const auto found = std::ranges::find(
        *object, name, [](const auto& entry) { return std::string_view{entry.first}; });
    return found == object->end() ? nullptr : &found->second;
}

auto text_field(const liquid::formats::json::value& value, std::string_view name)
    -> const std::string* {
    const auto* item = field(value, name);
    return item ? std::get_if<std::string>(&item->data) : nullptr;
}

auto base64(std::string_view input) -> liquid::result<std::vector<std::byte>> {
    const auto digit = [](char value) -> int {
        if (value >= 'A' && value <= 'Z') {
            return value - 'A';
        }
        if (value >= 'a' && value <= 'z') {
            return value - 'a' + 26;
        }
        if (value >= '0' && value <= '9') {
            return value - '0' + 52;
        }
        if (value == '+') {
            return 62;
        }
        if (value == '/') {
            return 63;
        }
        return -1;
    };
    if (input.size() % 4 != 0) {
        return std::unexpected(
            liquid::error{liquid::errc::invalid_argument, "invalid base64 length", "test"});
    }
    auto output = std::vector<std::byte>{};
    for (std::size_t offset = 0; offset < input.size(); offset += 4) {
        auto group = std::uint32_t{};
        auto bytes = 3U;
        for (auto index = 0U; index < 4; ++index) {
            group <<= 6U;
            if (input[offset + index] == '=') {
                --bytes;
                continue;
            }
            const auto value = digit(input[offset + index]);
            if (value < 0) {
                return std::unexpected(liquid::error{
                    liquid::errc::invalid_argument, "invalid base64 character", "test"});
            }
            group |= static_cast<std::uint32_t>(value);
        }
        output.push_back(std::byte{static_cast<unsigned char>((group >> 16U) & 0xffU)});
        if (bytes > 1) {
            output.push_back(std::byte{static_cast<unsigned char>((group >> 8U) & 0xffU)});
        }
        if (bytes > 2) {
            output.push_back(std::byte{static_cast<unsigned char>(group & 0xffU)});
        }
    }
    return output;
}

struct stored_provenance {
    temporary_repository location;
    liquid::repository repository;
    liquid::result_id result;
    liquid::action_result value;
    liquid::digest input_tree;
    liquid::digest toolchain_tree;

    stored_provenance() {
        auto opened = liquid::runtime::block_on(liquid::repository::create(location.path()));
        EXPECT_TRUE(opened) << opened.error().message;
        repository = std::move(*opened);
        auto tree = liquid::objects::encode(liquid::tree{});
        EXPECT_TRUE(tree) << tree.error().message;
        auto input = liquid::runtime::block_on(
            liquid::repository_detail::build_access::put_record(repository, *tree));
        EXPECT_TRUE(input) << input.error().message;
        auto toolchain = liquid::runtime::block_on(
            liquid::repository_detail::build_access::put_record(repository, *tree));
        EXPECT_TRUE(toolchain) << toolchain.error().message;
        input_tree = *input;
        toolchain_tree = *toolchain;
        const auto action =
            liquid::objects::action_record{.input = {.command = {"clang", "-c", "hello.c"},
                                                     .cwd = "src",
                                                     .env = {{"LANG", "C.UTF-8"}},
                                                     .outputs = {"out/hello.o"},
                                                     .inputs = input_tree,
                                                     .toolchain = toolchain_tree,
                                                     .platform = {"linux", "x86_64"}},
                                           .sandbox = example_digest(3)};
        value = {.exit = 0,
                 .outputs = {{"out/hello.o", 0644, example_digest(4)}},
                 .stdout_output = example_digest(5),
                 .stderr_output = example_digest(6),
                 .manifest = {{"hello.c", example_digest(7)}},
                 .worker = "linux/x86_64",
                 .observed_at = std::chrono::sys_seconds{std::chrono::seconds{100}}};
        value.started_at = std::chrono::sys_seconds{std::chrono::seconds{101}};
        value.finished_at = std::chrono::sys_seconds{std::chrono::seconds{102}};
        auto stored = liquid::runtime::block_on(
            liquid::build_detail::testing::store_result_with_provenance(repository, action, value));
        EXPECT_TRUE(stored) << stored.error().message;
        result = *stored;
    }
};

TEST(BuildProvenance, stores_and_verifies_the_slsa_mapping) {
    stored_provenance fixture;
    auto report = liquid::runtime::block_on(
        liquid::build::verify_provenance(fixture.repository, fixture.result));
    ASSERT_TRUE(report) << report.error().message;
    ASSERT_TRUE(report->verified) << report->reason;
    ASSERT_TRUE(report->value.provenance);
    EXPECT_EQ(liquid::to_string(*report->value.provenance), liquid::to_string(report->attestation));
    ASSERT_TRUE(report->statement.slsa_provenance);
    const auto& predicate = *report->statement.slsa_provenance;
    EXPECT_EQ(predicate.build_definition.external_parameters.command,
              (std::vector<std::string>{"clang", "-c", "hello.c"}));
    EXPECT_EQ(predicate.build_definition.external_parameters.cwd, "src");
    EXPECT_EQ(predicate.build_definition.external_parameters.environment.at("LANG"), "C.UTF-8");
    EXPECT_EQ(predicate.build_definition.internal_parameters.toolchain.uri,
              "liquid:tree:" + liquid::to_string(fixture.toolchain_tree));
    EXPECT_EQ(predicate.build_definition.internal_parameters.sandbox.uri,
              "liquid:sandbox:" + liquid::to_string(example_digest(3)));
    ASSERT_EQ(predicate.build_definition.resolved_dependencies.size(),
              fixture.value.manifest.size());
    EXPECT_EQ(predicate.build_definition.resolved_dependencies.front().uri,
              "liquid:blob:" + liquid::to_string(fixture.value.manifest.front().content));
    EXPECT_EQ(predicate.build_definition.resolved_dependencies.front().digest.at("blake3"),
              digest_hex(fixture.value.manifest.front().content));
    EXPECT_EQ(predicate.run_details.builder.id, report->value.producer.taap_id);
    EXPECT_EQ(predicate.run_details.metadata.started_on, "1970-01-01T00:01:41Z");
    EXPECT_EQ(predicate.run_details.metadata.finished_on, "1970-01-01T00:01:42Z");
}

TEST(BuildProvenance, signed_results_enumerate_content_addressed_output_claims) {
    stored_provenance fixture;
    auto outputs = liquid::runtime::block_on(liquid::build::known_outputs(fixture.repository));
    ASSERT_TRUE(outputs) << outputs.error().message;
    ASSERT_EQ(outputs->size(), 1U);
    ASSERT_TRUE(fixture.value.output_tree);
    EXPECT_EQ(liquid::to_string(outputs->front().input_tree),
              liquid::to_string(fixture.input_tree));
    EXPECT_EQ(liquid::to_string(outputs->front().output_tree),
              liquid::to_string(*fixture.value.output_tree));
}

TEST(BuildProvenance, tampered_predicate_fails_signature_verification) {
    stored_provenance fixture;
    auto report = liquid::runtime::block_on(
        liquid::build::verify_provenance(fixture.repository, fixture.result));
    ASSERT_TRUE(report);
    ASSERT_TRUE(report->statement.slsa_provenance);
    report->statement.slsa_provenance->build_definition.external_parameters.command.front() =
        "tampered";
    auto verified = liquid::verify_attestation(report->statement);
    ASSERT_TRUE(verified) << verified.error().message;
    EXPECT_FALSE(verified->verified);
    EXPECT_FALSE(verified->reason.empty());
}

TEST(BuildProvenance, dsse_export_has_in_toto_shape_and_preserves_the_stored_digest) {
    stored_provenance fixture;
    auto report = liquid::runtime::block_on(
        liquid::build::verify_provenance(fixture.repository, fixture.result));
    ASSERT_TRUE(report);
    ASSERT_TRUE(report->verified) << report->reason;
    auto exported = liquid::build::provenance_json(*report, true);
    ASSERT_TRUE(exported) << exported.error().message;
    auto envelope = liquid::formats::json::parse(*exported);
    ASSERT_TRUE(envelope) << envelope.error().message;
    ASSERT_NE(text_field(*envelope, "payloadType"), nullptr);
    EXPECT_EQ(*text_field(*envelope, "payloadType"), "application/vnd.in-toto+json");
    const auto* signatures = field(*envelope, "signatures");
    ASSERT_NE(signatures, nullptr);
    const auto* signature_values =
        std::get_if<liquid::formats::json::value::array>(&signatures->data);
    ASSERT_NE(signature_values, nullptr);
    ASSERT_EQ(signature_values->size(), 1U);
    EXPECT_NE(text_field(signature_values->front(), "keyid"), nullptr);
    EXPECT_NE(text_field(signature_values->front(), "sig"), nullptr);

    const auto* payload_text = text_field(*envelope, "payload");
    ASSERT_NE(payload_text, nullptr);
    auto payload_bytes = base64(*payload_text);
    ASSERT_TRUE(payload_bytes) << payload_bytes.error().message;
    const auto payload =
        std::string{reinterpret_cast<const char*>(payload_bytes->data()), payload_bytes->size()};
    auto statement = liquid::formats::json::parse(payload);
    ASSERT_TRUE(statement) << statement.error().message;
    ASSERT_NE(text_field(*statement, "_type"), nullptr);
    EXPECT_EQ(*text_field(*statement, "_type"), "https://in-toto.io/Statement/v1");
    ASSERT_NE(text_field(*statement, "predicateType"), nullptr);
    EXPECT_EQ(*text_field(*statement, "predicateType"), "https://slsa.dev/provenance/v1");
    EXPECT_NE(field(*statement, "subject"), nullptr);
    EXPECT_NE(field(*statement, "predicate"), nullptr);

    const auto* encoded_text = text_field(*envelope, "liquidAttestation");
    ASSERT_NE(encoded_text, nullptr);
    auto encoded = base64(*encoded_text);
    ASSERT_TRUE(encoded) << encoded.error().message;
    auto digest = liquid::hash(report->attestation.alg, *encoded);
    ASSERT_TRUE(digest) << digest.error().message;
    EXPECT_EQ(liquid::to_string(*digest), liquid::to_string(report->attestation));
    auto decoded = liquid::decode_attestation(*encoded);
    ASSERT_TRUE(decoded) << decoded.error().message;
    auto reencoded = liquid::encode_attestation(*decoded);
    ASSERT_TRUE(reencoded) << reencoded.error().message;
    EXPECT_EQ(*reencoded, *encoded);
}

} // namespace
