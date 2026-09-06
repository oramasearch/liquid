#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "../support/forge_double.hpp"
#include "../temporary_directory.hpp"
#include "build/repository_access.hpp"
#include "liquid/configuration.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "vfs/repository_access.hpp"

namespace {

constexpr auto zero_digest =
    "blake3:0000000000000000000000000000000000000000000000000000000000000000";

auto bytes(std::string_view value) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()),
            reinterpret_cast<const std::byte*>(value.data() + value.size())};
}

#if !defined(_WIN32)
class environment_guard {
public:
    environment_guard(std::string name, std::string value) : name_(std::move(name)) {
        if (const auto* previous = std::getenv(name_.c_str())) {
            previous_ = previous;
        }
        if (setenv(name_.c_str(), value.c_str(), 1) != 0) {
            throw std::runtime_error{"cannot set test environment"};
        }
    }

    ~environment_guard() {
        if (previous_) {
            static_cast<void>(setenv(name_.c_str(), previous_->c_str(), 1));
        } else {
            static_cast<void>(unsetenv(name_.c_str()));
        }
    }

private:
    std::string name_;
    std::optional<std::string> previous_;
};
#endif

#if LIQUID_BUILD_HAS_FUSE
class configuration_action_fixture {
public:
    explicit configuration_action_fixture(std::string name) {
        path_ = liquid::test::fixture_path(std::move(name));
        auto created = liquid::runtime::block_on(liquid::repository::create(path_));
        if (!created) {
            throw std::runtime_error{created.error().message};
        }
        repository = std::move(*created);
        expect(repository.write("input.txt", bytes("configuration target input")));
        expect(repository.write(
            "generate.sh",
            bytes("#!/bin/sh\nprintf '%s\\n' 'package generated { versions: [2.4.1] }' > "
                  "generated.liquid\n")));
        expect(repository.write_file("bin/fixture", LIQUID_BUILD_FIXTURE));
        expect(repository.write_file("bin/sh", "/bin/sh"));
#if defined(__x86_64__)
        expect(repository.write_file("lib/x86_64-linux-gnu/libc.so.6",
                                     "/lib/x86_64-linux-gnu/libc.so.6"));
        expect(repository.write_file("lib64/ld-linux-x86-64.so.2", "/lib64/ld-linux-x86-64.so.2"));
#elif defined(__aarch64__)
        expect(repository.write_file("lib/aarch64-linux-gnu/libc.so.6",
                                     "/lib/aarch64-linux-gnu/libc.so.6"));
        expect(repository.write_file("lib/ld-linux-aarch64.so.1", "/lib/ld-linux-aarch64.so.1"));
#endif
        expect(liquid::repository_detail::vfs_access::set_mode(repository, "bin/fixture", 0100755));
        expect(liquid::repository_detail::vfs_access::set_mode(repository, "bin/sh", 0100755));
        expect(repository.cut("configuration action fixture"));
        auto log = liquid::runtime::block_on(repository.log());
        if (!log || log->empty()) {
            throw std::runtime_error{"configuration action fixture has no root"};
        }
        root = log->front().root;
    }

    liquid::repository repository;
    liquid::digest root;

private:
    template <class Task> static void expect(Task task) {
        auto result = liquid::runtime::block_on(std::move(task));
        if (!result) {
            throw std::runtime_error{result.error().message};
        }
    }

    std::filesystem::path path_;
};
#endif

auto diagnostics(std::string_view text) -> std::vector<std::string> {
    auto parsed = liquid::parse_configuration(text, "errors.liquid");
    if (!parsed) {
        return {parsed.error().message};
    }
    auto output = std::vector<std::string>{};
    for (const auto& item : liquid::check(*parsed)) {
        output.push_back(liquid::to_string(item));
    }
    return output;
}

TEST(ConfigurationErrors, twenty_golden_diagnostics_are_exact) {
    struct golden {
        std::string input;
        std::string expected;
    };
    const auto cases = std::array{
        golden{"package { versions: [1.2.3] }",
               "errors.liquid:1:1: expected package name, found missing name\n"
               "  hint: write package <name> { ... }"},
        golden{"package p {}",
               "errors.liquid:1:1: expected list of versions, found missing field 'versions'\n"
               "  hint: packages require versions: [1.14.3]"},
        golden{"package p { versions: [\"1.2.3\"] }",
               "errors.liquid:1:24: expected version, found string '1.2.3'\n"
               "  hint: versions are unquoted: 1.14.3"},
        golden{"package p { versions: [1.2.3] commnad: [] }",
               "errors.liquid:1:31: expected known package field, found 'commnad'\n"
               "  hint: did you mean 'compilers'?"},
        golden{"build { command: [\"cc\"] }",
               "errors.liquid:1:1: expected build name, found missing name\n"
               "  hint: write build <name> { ... }"},
        golden{"build app {}",
               "errors.liquid:1:1: expected list of strings, found missing field 'command'\n"
               "  hint: commands are argv lists: [\"cc\", \"main.c\"]"},
        golden{"build app { command: \"cc\" }",
               "errors.liquid:1:22: expected list, found string 'cc'\n"
               "  hint: lists use brackets: [\"one\", \"two\"]"},
        golden{"build app { command: [\"cc\"] environment: { A: 3 } }",
               "errors.liquid:1:47: expected string environment value, found integer 3\n"
               "  hint: strings are quoted: \"value\""},
        golden{"build app { command: [\"cc\"] sources: [true] }",
               "errors.liquid:1:39: expected source name or digest, found boolean true\n"
               "  hint: sources are quoted names or declared digests"},
        golden{"build app { command: [\"cc\"] dependencies: [false] }",
               "errors.liquid:1:44: expected string dependency name, found boolean false\n"
               "  hint: strings are quoted: \"value\""},
        golden{"build app { command: [\"cc\"] outputs: [7] }",
               "errors.liquid:1:39: expected path output, found integer 7\n"
               "  hint: paths use the path literal: path \"relative/path\""},
        golden{"build app { command: [\"cc\"] cwd: 9 }",
               "errors.liquid:1:34: expected path, found integer 9\n"
               "  hint: paths use the path literal: path \"relative/path\""},
        golden{"build app { command: [\"cc\"] toolchain: \"bad\" }",
               "errors.liquid:1:40: expected digest, found string 'bad'\n"
               "  hint: digests include the algorithm: blake3:<64 hexadecimal digits>"},
        golden{"source { url: \"https://example.test/a\" digest: " + std::string{zero_digest} +
                   " kind: file }",
               "errors.liquid:1:1: expected source name, found missing name\n"
               "  hint: write source <name> { ... }"},
        golden{"source archive { digest: " + std::string{zero_digest} + " kind: archive }",
               "errors.liquid:1:1: expected string URL, found missing field 'url'\n"
               "  hint: sources require an https URL"},
        golden{"source archive { url: \"https://example.test/a\" digest: \"bad\" kind: archive }",
               "errors.liquid:1:56: expected digest, found string 'bad'\n"
               "  hint: digests include the algorithm: blake3:<64 hexadecimal digits>"},
        golden{"source archive { url: \"https://example.test/a\" digest: " +
                   std::string{zero_digest} + " kind: \"file\" }",
               "errors.liquid:1:134: expected identifier, found string 'file'\n"
               "  hint: enum values are unquoted"},
        golden{"source archive { url: \"https://example.test/a\" digest: " +
                   std::string{zero_digest} + " kind: git }",
               "errors.liquid:1:134: expected archive or file, found identifier 'git'\n"
               "  hint: source kind is archive or file"},
        golden{"package p { versions: [1.2.3] variant { values: [true] default: true } }",
               "errors.liquid:1:31: expected variant name, found missing name\n"
               "  hint: write variant <name> { ... }"},
        golden{"package p { versions: [1.2.3] depends_on z { type: host } }",
               "errors.liquid:1:52: expected build, link, or run, found identifier 'host'\n"
               "  hint: dependency types are build, link, and run"}};

    for (const auto& item : cases) {
        const auto actual = diagnostics(item.input);
        ASSERT_FALSE(actual.empty()) << item.input;
        EXPECT_EQ(actual.front(), item.expected) << item.input;
    }
}

TEST(ConfigurationLanguage, package_target_source_and_when_lower_to_typed_records) {
    const auto text = std::string{"source zlib {\n"
                                  "  url: \"https://example.test/zlib.tar.gz\"\n"
                                  "  digest: "} +
                      zero_digest +
                      "\n"
                      "  kind: archive\n"
                      "}\n"
                      "package hdf5 {\n"
                      "  versions: [1.14.3]\n"
                      "  compilers: [\"gcc@13.2\"]\n"
                      "  targets: [\"zen4\"]\n"
                      "  variant mpi { values: [true, false] default: false }\n"
                      "  variant fortran { values: [true, false] default: false }\n"
                      "  when \"hdf5 +mpi\" { depends_on openmpi { version: 5.0.0 type: link } }\n"
                      "}\n"
                      "build hdf5 {\n"
                      "  command: [\"cc\", \"main.c\", \"-o\", \"build/hdf5\"]\n"
                      "  sources: [\"zlib\"]\n"
                      "  outputs: [path \"build/hdf5\"]\n"
                      "  when \"hdf5 +mpi\" { depends_on openmpi { type: link } }\n"
                      "  install { layout: prefix prefix: path \"install\" }\n"
                      "}\n";
    auto parsed = liquid::parse_configuration(text, "package.liquid");
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_TRUE(liquid::check(*parsed).empty());
    auto lowered = liquid::lower(*parsed);
    ASSERT_TRUE(lowered) << lowered.error().message;
    ASSERT_EQ(lowered->packages.size(), 1U);
    EXPECT_EQ(lowered->packages.front().name, "hdf5");
    EXPECT_EQ(lowered->packages.front().versions.front().version, "1.14.3");
    EXPECT_EQ(lowered->packages.front().compiler_constraints,
              (std::vector<std::string>{"gcc@13.2"}));
    EXPECT_EQ(lowered->packages.front().target_constraints, (std::vector<std::string>{"zen4"}));
    ASSERT_EQ(lowered->packages.front().variants.size(), 2U);
    EXPECT_EQ(lowered->packages.front().variants.front().name, "mpi");
    EXPECT_EQ(lowered->packages.front().variants.front().default_value, "false");
    ASSERT_EQ(lowered->packages.front().dependencies.size(), 1U);
    EXPECT_EQ(lowered->packages.front().dependencies.front().when, "hdf5 +mpi");
    EXPECT_EQ(lowered->packages.front().dependencies.front().version, "5.0.0");
    EXPECT_EQ(lowered->packages.front().dependencies.front().type, liquid::dependency_type::link);
    ASSERT_EQ(lowered->targets.size(), 1U);
    EXPECT_EQ(lowered->targets.front().sources.size(), 1U);
    ASSERT_EQ(lowered->targets.front().dependencies.size(), 1U);
    EXPECT_EQ(lowered->targets.front().dependencies.front().when, "hdf5 +mpi");
    EXPECT_EQ(lowered->targets.front().install_layout, "prefix");
}

TEST(ConfigurationLanguage, includes_are_local_and_lower_with_the_document) {
    const auto root = liquid::test::fixture_path("liquid-configuration-includes");
    auto code = std::error_code{};
    std::filesystem::create_directories(root / "rules", code);
    ASSERT_FALSE(code) << code.message();
    auto included = std::ofstream{root / "rules/package.liquid", std::ios::trunc};
    included << "package included { versions: [3.2.1] }\n";
    included.close();
    ASSERT_TRUE(included);

    auto document =
        liquid::parse_configuration("include \"rules/package.liquid\"\n", root / "liquid.liquid");
    ASSERT_TRUE(document) << document.error().message;
    auto lowered = liquid::lower(*document);
    ASSERT_TRUE(lowered) << lowered.error().message;
    ASSERT_EQ(lowered->packages.size(), 1U);
    EXPECT_EQ(lowered->packages.front().name, "included");

    auto remote = liquid::parse_configuration("include \"https://example.test/package.liquid\"\n",
                                              root / "remote.liquid");
    ASSERT_FALSE(remote);
    EXPECT_NE(remote.error().message.find("remote and parent-directory includes are forbidden"),
              std::string::npos);
    auto parent =
        liquid::parse_configuration("include \"../package.liquid\"\n", root / "parent.liquid");
    ASSERT_FALSE(parent);
    EXPECT_NE(parent.error().message.find("remote and parent-directory includes are forbidden"),
              std::string::npos);
}

TEST(ConfigurationLanguage, source_urls_require_https) {
    const auto text = std::string{"source archive { url: \"http://example.test/a\" digest: "} +
                      zero_digest + " kind: archive }";
    const auto actual = diagnostics(text);
    ASSERT_EQ(actual.size(), 1U);
    EXPECT_EQ(actual.front(),
              "errors.liquid:1:23: expected HTTPS URL, found string 'http://example.test/a'\n"
              "  hint: source URLs begin with https://");
}

TEST(ConfigurationLanguage, canonical_formatter_is_idempotent) {
    const auto text = "build \"my app\"{environment:{\"A B\":\"value\"} "
                      "outputs:[path \"app\"] command:[\"cc\",\"main.c\"]}";
    auto parsed = liquid::parse_configuration(text, "format.liquid");
    ASSERT_TRUE(parsed) << parsed.error().message;
    auto formatted = liquid::format_configuration(*parsed);
    ASSERT_TRUE(formatted) << formatted.error().message;
    auto reparsed = liquid::parse_configuration(*formatted, "format.liquid");
    ASSERT_TRUE(reparsed) << reparsed.error().message;
    auto second = liquid::format_configuration(*reparsed);
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_EQ(*second, *formatted);
    EXPECT_NE(formatted->find("build \"my app\""), std::string::npos);
    EXPECT_NE(formatted->find("\"A B\": \"value\""), std::string::npos);
}

TEST(ConfigurationRecords, target_and_source_use_canonical_cbor) {
    auto parsed_digest = liquid::parse_digest(zero_digest);
    ASSERT_TRUE(parsed_digest) << parsed_digest.error().message;
    const auto origin = liquid::configuration_record_origin{
        .kind = liquid::configuration_origin_kind::source, .file = "records.liquid"};
    const auto source = liquid::source{.name = "archive",
                                       .url = "https://example.test/archive.tar",
                                       .content = *parsed_digest,
                                       .kind = liquid::source_kind::archive,
                                       .origin = origin};
    auto encoded_source = liquid::encode_source(source);
    ASSERT_TRUE(encoded_source) << encoded_source.error().message;
    auto decoded_source = liquid::decode_source(*encoded_source);
    ASSERT_TRUE(decoded_source) << decoded_source.error().message;
    auto reencoded_source = liquid::encode_source(*decoded_source);
    ASSERT_TRUE(reencoded_source) << reencoded_source.error().message;
    EXPECT_EQ(*reencoded_source, *encoded_source);

    const auto target = liquid::target{.name = "app",
                                       .command = {"cc", "main.c"},
                                       .environment = {{"LANG", "C"}},
                                       .sources = {*parsed_digest},
                                       .dependencies = {{"zlib", "app +zlib"}},
                                       .outputs = {"app"},
                                       .origin = origin};
    auto encoded_target = liquid::encode_target(target);
    ASSERT_TRUE(encoded_target) << encoded_target.error().message;
    auto decoded_target = liquid::decode_target(*encoded_target);
    ASSERT_TRUE(decoded_target) << decoded_target.error().message;
    auto reencoded_target = liquid::encode_target(*decoded_target);
    ASSERT_TRUE(reencoded_target) << reencoded_target.error().message;
    EXPECT_EQ(*reencoded_target, *encoded_target);
}

#if !defined(_WIN32)
TEST(ConfigurationSources, digest_mismatch_never_enters_the_store) {
    const auto certificate = environment_guard{"SSL_CERT_FILE", LIQUID_TLS_CERTIFICATE};
    auto server = liquid::test::forge_double{
        {}, "127.0.0.1:0", LIQUID_TLS_CERTIFICATE, LIQUID_TLS_PRIVATE_KEY};
    auto url = server.url();
    url.resize(url.find("/v1/"));
    url += "/v1/list_reviews?repository_id=test&limit=1000";

    const auto path = liquid::test::fixture_path("liquid-configuration-source-mismatch");
    auto repository = liquid::runtime::block_on(liquid::repository::create(path));
    ASSERT_TRUE(repository) << repository.error().message;
    auto declared = liquid::parse_digest(zero_digest);
    ASSERT_TRUE(declared) << declared.error().message;
    const auto value = liquid::source{.name = "mismatch",
                                      .url = std::move(url),
                                      .content = *declared,
                                      .kind = liquid::source_kind::file,
                                      .origin = {.file = "source.liquid"}};
    auto fetched = liquid::runtime::block_on(repository->fetch_source(value));
    ASSERT_FALSE(fetched);
    EXPECT_EQ(fetched.error().code, liquid::errc::corrupt);

    const auto marker = std::string_view{"found "};
    const auto position = fetched.error().message.find(marker);
    ASSERT_NE(position, std::string::npos);
    auto actual = liquid::parse_digest(fetched.error().message.substr(position + marker.size()));
    ASSERT_TRUE(actual) << actual.error().message;
    auto absent = liquid::runtime::block_on(
        liquid::repository_detail::build_access::get(*repository, *actual));
    EXPECT_FALSE(absent);
}
#endif

#if LIQUID_BUILD_HAS_FUSE
TEST(ConfigurationGenerators, shell_output_lowers_with_verified_result_origin) {
    auto fixture = configuration_action_fixture{"liquid-configuration-generator"};
    const auto root = liquid::to_string(fixture.root);
    const auto text = std::string{"generate = build {\n"} +
                      "  command: [\"sh\", \"generate.sh\"]\n" + "  inputs: " + root + "\n" +
                      "  toolchain: " + root + "\n" + "  output: path \"generated.liquid\"\n" +
                      "  outputs: [path \"generated.liquid\"]\n" + "}\n";
    auto document = liquid::parse_configuration(text, "liquid.liquid");
    ASSERT_TRUE(document) << document.error().message;
    auto lowered =
        liquid::runtime::block_on(liquid::run_generators(fixture.repository, std::move(*document)));
    ASSERT_TRUE(lowered) << lowered.error().message;
    ASSERT_EQ(lowered->packages.size(), 1U);
    ASSERT_EQ(lowered->package_origins.size(), 1U);
    EXPECT_EQ(lowered->packages.front().name, "generated");
    EXPECT_EQ(lowered->package_origins.front().kind, liquid::configuration_origin_kind::generated);
    ASSERT_TRUE(lowered->package_origins.front().result);
    auto provenance = liquid::runtime::block_on(liquid::build::verify_provenance(
        fixture.repository, liquid::result_id{*lowered->package_origins.front().result}));
    ASSERT_TRUE(provenance) << provenance.error().message;
    EXPECT_TRUE(provenance->verified) << provenance->reason;
}

TEST(ConfigurationTargets, configured_target_executes_through_the_action_path) {
    auto fixture = configuration_action_fixture{"liquid-configuration-target"};
    const auto certificate = environment_guard{"SSL_CERT_FILE", LIQUID_TLS_CERTIFICATE};
    auto server = liquid::test::forge_double{
        {}, "127.0.0.1:0", LIQUID_TLS_CERTIFICATE, LIQUID_TLS_PRIVATE_KEY};
    auto url = server.url();
    url.resize(url.find("/v1/"));
    url += "/v1/list_reviews?repository_id=test&limit=1000";
    auto zero = liquid::parse_digest(zero_digest);
    ASSERT_TRUE(zero) << zero.error().message;
    auto mismatched = liquid::runtime::block_on(fixture.repository.fetch_source(
        {.name = "payload", .url = url, .content = *zero, .kind = liquid::source_kind::file}));
    ASSERT_FALSE(mismatched);
    const auto marker = std::string_view{"found "};
    const auto position = mismatched.error().message.find(marker);
    ASSERT_NE(position, std::string::npos);
    const auto content_text = mismatched.error().message.substr(position + marker.size());
    const auto root = liquid::to_string(fixture.root);
    auto source_path = content_text;
    std::ranges::replace(source_path, ':', '/');
    const auto text =
        std::string{"source payload {\n"} + "  url: \"" + url + "\"\n" +
        "  digest: " + content_text + "\n" + "  kind: file\n" + "}\n" + "build app {\n" +
        "  command: [\"fixture\", \"copy\", "
        "\"deps/__liquid_sources/" +
        source_path + "\", \"out/app\"]\n" + "  sources: [\"payload\"]\n" + "  inputs: " + root +
        "\n" + "  toolchain: " + root + "\n" + "  outputs: [path \"out/app\"]\n" + "}\n";
    auto document = liquid::parse_configuration(text, "liquid.liquid");
    ASSERT_TRUE(document) << document.error().message;
    auto configuration = liquid::lower(*document);
    ASSERT_TRUE(configuration) << configuration.error().message;
    auto built = liquid::runtime::block_on(
        liquid::build_configuration(fixture.repository, *configuration, "app"));
    ASSERT_TRUE(built) << built.error().message;
    auto result = liquid::runtime::block_on(liquid::build::result(fixture.repository, *built));
    ASSERT_TRUE(result) << result.error().message;
    ASSERT_EQ(result->outputs.size(), 1U);
    EXPECT_EQ(result->outputs.front().path, "out/app");
    auto provenance =
        liquid::runtime::block_on(liquid::build::verify_provenance(fixture.repository, *built));
    ASSERT_TRUE(provenance) << provenance.error().message;
    EXPECT_TRUE(provenance->verified) << provenance->reason;
}
#endif

} // namespace
